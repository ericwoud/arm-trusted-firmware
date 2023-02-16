/*
 * Copyright (c) 2013-2023, Arm Limited and Contributors. All rights reserved.
 * Copyright (c) 2022, NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <platform_def.h>

#include <arch.h>
#include <arch_helpers.h>
#include <arch_features.h>
#include <bl31/interrupt_mgmt.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <context.h>
#include <drivers/arm/gicv3.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/el3_runtime/pubsub_events.h>
#include <lib/extensions/amu.h>
#include <lib/extensions/brbe.h>
#include <lib/extensions/mpam.h>
#include <lib/extensions/pmuv3.h>
#include <lib/extensions/sme.h>
#include <lib/extensions/spe.h>
#include <lib/extensions/sve.h>
#include <lib/extensions/sys_reg_trace.h>
#include <lib/extensions/trbe.h>
#include <lib/extensions/trf.h>
#include <lib/utils.h>

#if ENABLE_FEAT_TWED
/* Make sure delay value fits within the range(0-15) */
CASSERT(((TWED_DELAY & ~SCR_TWEDEL_MASK) == 0U), assert_twed_delay_value_check);
#endif /* ENABLE_FEAT_TWED */

static void manage_extensions_nonsecure(cpu_context_t *ctx);
static void manage_extensions_secure(cpu_context_t *ctx);

static void setup_el1_context(cpu_context_t *ctx, const struct entry_point_info *ep)
{
	u_register_t sctlr_elx, actlr_elx;

	/*
	 * Initialise SCTLR_EL1 to the reset value corresponding to the target
	 * execution state setting all fields rather than relying on the hw.
	 * Some fields have architecturally UNKNOWN reset values and these are
	 * set to zero.
	 *
	 * SCTLR.EE: Endianness is taken from the entrypoint attributes.
	 *
	 * SCTLR.M, SCTLR.C and SCTLR.I: These fields must be zero (as
	 * required by PSCI specification)
	 */
	sctlr_elx = (EP_GET_EE(ep->h.attr) != 0U) ? SCTLR_EE_BIT : 0UL;
	if (GET_RW(ep->spsr) == MODE_RW_64) {
		sctlr_elx |= SCTLR_EL1_RES1;
	} else {
		/*
		 * If the target execution state is AArch32 then the following
		 * fields need to be set.
		 *
		 * SCTRL_EL1.nTWE: Set to one so that EL0 execution of WFE
		 *  instructions are not trapped to EL1.
		 *
		 * SCTLR_EL1.nTWI: Set to one so that EL0 execution of WFI
		 *  instructions are not trapped to EL1.
		 *
		 * SCTLR_EL1.CP15BEN: Set to one to enable EL0 execution of the
		 *  CP15DMB, CP15DSB, and CP15ISB instructions.
		 */
		sctlr_elx |= SCTLR_AARCH32_EL1_RES1 | SCTLR_CP15BEN_BIT
					| SCTLR_NTWI_BIT | SCTLR_NTWE_BIT;
	}

#if ERRATA_A75_764081
	/*
	 * If workaround of errata 764081 for Cortex-A75 is used then set
	 * SCTLR_EL1.IESB to enable Implicit Error Synchronization Barrier.
	 */
	sctlr_elx |= SCTLR_IESB_BIT;
#endif
	/* Store the initialised SCTLR_EL1 value in the cpu_context */
	write_ctx_reg(get_el1_sysregs_ctx(ctx), CTX_SCTLR_EL1, sctlr_elx);

	/*
	 * Base the context ACTLR_EL1 on the current value, as it is
	 * implementation defined. The context restore process will write
	 * the value from the context to the actual register and can cause
	 * problems for processor cores that don't expect certain bits to
	 * be zero.
	 */
	actlr_elx = read_actlr_el1();
	write_ctx_reg((get_el1_sysregs_ctx(ctx)), (CTX_ACTLR_EL1), (actlr_elx));
}

/******************************************************************************
 * This function performs initializations that are specific to SECURE state
 * and updates the cpu context specified by 'ctx'.
 *****************************************************************************/
static void setup_secure_context(cpu_context_t *ctx, const struct entry_point_info *ep)
{
	u_register_t scr_el3;
	el3_state_t *state;

	state = get_el3state_ctx(ctx);
	scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);

#if defined(IMAGE_BL31) && !defined(SPD_spmd)
	/*
	 * SCR_EL3.IRQ, SCR_EL3.FIQ: Enable the physical FIQ and IRQ routing as
	 * indicated by the interrupt routing model for BL31.
	 */
	scr_el3 |= get_scr_el3_from_routing_model(SECURE);
#endif

#if !CTX_INCLUDE_MTE_REGS || ENABLE_ASSERTIONS
	/* Get Memory Tagging Extension support level */
	unsigned int mte = get_armv8_5_mte_support();
#endif
	/*
	 * Allow access to Allocation Tags when CTX_INCLUDE_MTE_REGS
	 * is set, or when MTE is only implemented at EL0.
	 */
#if CTX_INCLUDE_MTE_REGS
	assert((mte == MTE_IMPLEMENTED_ELX) || (mte == MTE_IMPLEMENTED_ASY));
	scr_el3 |= SCR_ATA_BIT;
#else
	if (mte == MTE_IMPLEMENTED_EL0) {
		scr_el3 |= SCR_ATA_BIT;
	}
#endif /* CTX_INCLUDE_MTE_REGS */

	/* Enable S-EL2 if the next EL is EL2 and S-EL2 is present */
	if ((GET_EL(ep->spsr) == MODE_EL2) && is_feat_sel2_supported()) {
		if (GET_RW(ep->spsr) != MODE_RW_64) {
			ERROR("S-EL2 can not be used in AArch32\n.");
			panic();
		}

		scr_el3 |= SCR_EEL2_BIT;
	}

	write_ctx_reg(state, CTX_SCR_EL3, scr_el3);

	/*
	 * Initialize EL1 context registers unless SPMC is running
	 * at S-EL2.
	 */
#if !SPMD_SPM_AT_SEL2
	setup_el1_context(ctx, ep);
#endif

	manage_extensions_secure(ctx);
}

#if ENABLE_RME
/******************************************************************************
 * This function performs initializations that are specific to REALM state
 * and updates the cpu context specified by 'ctx'.
 *****************************************************************************/
static void setup_realm_context(cpu_context_t *ctx, const struct entry_point_info *ep)
{
	u_register_t scr_el3;
	el3_state_t *state;

	state = get_el3state_ctx(ctx);
	scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);

	scr_el3 |= SCR_NS_BIT | SCR_NSE_BIT;

	if (is_feat_csv2_2_supported()) {
		/* Enable access to the SCXTNUM_ELx registers. */
		scr_el3 |= SCR_EnSCXT_BIT;
	}

	write_ctx_reg(state, CTX_SCR_EL3, scr_el3);
}
#endif /* ENABLE_RME */

/******************************************************************************
 * This function performs initializations that are specific to NON-SECURE state
 * and updates the cpu context specified by 'ctx'.
 *****************************************************************************/
static void setup_ns_context(cpu_context_t *ctx, const struct entry_point_info *ep)
{
	u_register_t scr_el3;
	el3_state_t *state;

	state = get_el3state_ctx(ctx);
	scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);

	/* SCR_NS: Set the NS bit */
	scr_el3 |= SCR_NS_BIT;

#if !CTX_INCLUDE_PAUTH_REGS
	/*
	 * If the pointer authentication registers aren't saved during world
	 * switches the value of the registers can be leaked from the Secure to
	 * the Non-secure world. To prevent this, rather than enabling pointer
	 * authentication everywhere, we only enable it in the Non-secure world.
	 *
	 * If the Secure world wants to use pointer authentication,
	 * CTX_INCLUDE_PAUTH_REGS must be set to 1.
	 */
	scr_el3 |= SCR_API_BIT | SCR_APK_BIT;
#endif /* !CTX_INCLUDE_PAUTH_REGS */

	/* Allow access to Allocation Tags when MTE is implemented. */
	scr_el3 |= SCR_ATA_BIT;

#if HANDLE_EA_EL3_FIRST_NS
	/* SCR_EL3.EA: Route External Abort and SError Interrupt to EL3. */
	scr_el3 |= SCR_EA_BIT;
#endif

#if RAS_TRAP_NS_ERR_REC_ACCESS
	/*
	 * SCR_EL3.TERR: Trap Error record accesses. Accesses to the RAS ERR
	 * and RAS ERX registers from EL1 and EL2(from any security state)
	 * are trapped to EL3.
	 * Set here to trap only for NS EL1/EL2
	 *
	 */
	scr_el3 |= SCR_TERR_BIT;
#endif

	if (is_feat_csv2_2_supported()) {
		/* Enable access to the SCXTNUM_ELx registers. */
		scr_el3 |= SCR_EnSCXT_BIT;
	}

#ifdef IMAGE_BL31
	/*
	 * SCR_EL3.IRQ, SCR_EL3.FIQ: Enable the physical FIQ and IRQ routing as
	 *  indicated by the interrupt routing model for BL31.
	 */
	scr_el3 |= get_scr_el3_from_routing_model(NON_SECURE);
#endif
	write_ctx_reg(state, CTX_SCR_EL3, scr_el3);

	/* Initialize EL1 context registers */
	setup_el1_context(ctx, ep);

	/* Initialize EL2 context registers */
#if CTX_INCLUDE_EL2_REGS

	/*
	 * Initialize SCTLR_EL2 context register using Endianness value
	 * taken from the entrypoint attribute.
	 */
	u_register_t sctlr_el2 = (EP_GET_EE(ep->h.attr) != 0U) ? SCTLR_EE_BIT : 0UL;
	sctlr_el2 |= SCTLR_EL2_RES1;
	write_ctx_reg(get_el2_sysregs_ctx(ctx), CTX_SCTLR_EL2,
			sctlr_el2);

	/*
	 * Program the ICC_SRE_EL2 to make sure the correct bits are set
	 * when restoring NS context.
	 */
	u_register_t icc_sre_el2 = ICC_SRE_DIB_BIT | ICC_SRE_DFB_BIT |
				   ICC_SRE_EN_BIT | ICC_SRE_SRE_BIT;
	write_ctx_reg(get_el2_sysregs_ctx(ctx), CTX_ICC_SRE_EL2,
			icc_sre_el2);

	if (is_feat_hcx_supported()) {
		/*
		 * Initialize register HCRX_EL2 with its init value.
		 * As the value of HCRX_EL2 is UNKNOWN on reset, there is a
		 * chance that this can lead to unexpected behavior in lower
		 * ELs that have not been updated since the introduction of
		 * this feature if not properly initialized, especially when
		 * it comes to those bits that enable/disable traps.
		 */
		write_ctx_reg(get_el2_sysregs_ctx(ctx), CTX_HCRX_EL2,
			HCRX_EL2_INIT_VAL);
	}
#endif /* CTX_INCLUDE_EL2_REGS */

	manage_extensions_nonsecure(ctx);
}

/*******************************************************************************
 * The following function performs initialization of the cpu_context 'ctx'
 * for first use that is common to all security states, and sets the
 * initial entrypoint state as specified by the entry_point_info structure.
 *
 * The EE and ST attributes are used to configure the endianness and secure
 * timer availability for the new execution context.
 ******************************************************************************/
static void setup_context_common(cpu_context_t *ctx, const entry_point_info_t *ep)
{
	u_register_t scr_el3;
	el3_state_t *state;
	gp_regs_t *gp_regs;

	/* Clear any residual register values from the context */
	zeromem(ctx, sizeof(*ctx));

	/*
	 * SCR_EL3 was initialised during reset sequence in macro
	 * el3_arch_init_common. This code modifies the SCR_EL3 fields that
	 * affect the next EL.
	 *
	 * The following fields are initially set to zero and then updated to
	 * the required value depending on the state of the SPSR_EL3 and the
	 * Security state and entrypoint attributes of the next EL.
	 */
	scr_el3 = read_scr();
	scr_el3 &= ~(SCR_NS_BIT | SCR_RW_BIT | SCR_EA_BIT | SCR_FIQ_BIT | SCR_IRQ_BIT |
			SCR_ST_BIT | SCR_HCE_BIT | SCR_NSE_BIT);

	/*
	 * SCR_EL3.RW: Set the execution state, AArch32 or AArch64, for next
	 *  Exception level as specified by SPSR.
	 */
	if (GET_RW(ep->spsr) == MODE_RW_64) {
		scr_el3 |= SCR_RW_BIT;
	}

	/*
	 * SCR_EL3.ST: Traps Secure EL1 accesses to the Counter-timer Physical
	 * Secure timer registers to EL3, from AArch64 state only, if specified
	 * by the entrypoint attributes. If SEL2 is present and enabled, the ST
	 * bit always behaves as 1 (i.e. secure physical timer register access
	 * is not trapped)
	 */
	if (EP_GET_ST(ep->h.attr) != 0U) {
		scr_el3 |= SCR_ST_BIT;
	}

	/*
	 * If FEAT_HCX is enabled, enable access to HCRX_EL2 by setting
	 * SCR_EL3.HXEn.
	 */
	if (is_feat_hcx_supported()) {
		scr_el3 |= SCR_HXEn_BIT;
	}

	/*
	 * If FEAT_RNG_TRAP is enabled, all reads of the RNDR and RNDRRS
	 * registers are trapped to EL3.
	 */
#if ENABLE_FEAT_RNG_TRAP
	scr_el3 |= SCR_TRNDR_BIT;
#endif

#if FAULT_INJECTION_SUPPORT
	/* Enable fault injection from lower ELs */
	scr_el3 |= SCR_FIEN_BIT;
#endif

	/*
	 * SCR_EL3.TCR2EN: Enable access to TCR2_ELx for AArch64 if present.
	 */
	if (is_feat_tcr2_supported() && (GET_RW(ep->spsr) == MODE_RW_64)) {
		scr_el3 |= SCR_TCR2EN_BIT;
	}

	/*
	 * SCR_EL3.PIEN: Enable permission indirection and overlay
	 * registers for AArch64 if present.
	 */
	if (is_feat_sxpie_supported() || is_feat_sxpoe_supported()) {
		scr_el3 |= SCR_PIEN_BIT;
	}

	/*
	 * SCR_EL3.GCSEn: Enable GCS registers for AArch64 if present.
	 */
	if ((is_feat_gcs_supported()) && (GET_RW(ep->spsr) == MODE_RW_64)) {
		scr_el3 |= SCR_GCSEn_BIT;
	}

	/*
	 * CPTR_EL3 was initialized out of reset, copy that value to the
	 * context register.
	 */
	write_ctx_reg(get_el3state_ctx(ctx), CTX_CPTR_EL3, read_cptr_el3());

	/*
	 * SCR_EL3.HCE: Enable HVC instructions if next execution state is
	 * AArch64 and next EL is EL2, or if next execution state is AArch32 and
	 * next mode is Hyp.
	 * SCR_EL3.FGTEn: Enable Fine Grained Virtualization Traps under the
	 * same conditions as HVC instructions and when the processor supports
	 * ARMv8.6-FGT.
	 * SCR_EL3.ECVEn: Enable Enhanced Counter Virtualization (ECV)
	 * CNTPOFF_EL2 register under the same conditions as HVC instructions
	 * and when the processor supports ECV.
	 */
	if (((GET_RW(ep->spsr) == MODE_RW_64) && (GET_EL(ep->spsr) == MODE_EL2))
	    || ((GET_RW(ep->spsr) != MODE_RW_64)
		&& (GET_M32(ep->spsr) == MODE32_hyp))) {
		scr_el3 |= SCR_HCE_BIT;

		if (is_feat_fgt_supported()) {
			scr_el3 |= SCR_FGTEN_BIT;
		}

		if (is_feat_ecv_supported()) {
			scr_el3 |= SCR_ECVEN_BIT;
		}
	}

	/* Enable WFE trap delay in SCR_EL3 if supported and configured */
	if (is_feat_twed_supported()) {
		/* Set delay in SCR_EL3 */
		scr_el3 &= ~(SCR_TWEDEL_MASK << SCR_TWEDEL_SHIFT);
		scr_el3 |= ((TWED_DELAY & SCR_TWEDEL_MASK)
				<< SCR_TWEDEL_SHIFT);

		/* Enable WFE delay */
		scr_el3 |= SCR_TWEDEn_BIT;
	}

	/*
	 * Populate EL3 state so that we've the right context
	 * before doing ERET
	 */
	state = get_el3state_ctx(ctx);
	write_ctx_reg(state, CTX_SCR_EL3, scr_el3);
	write_ctx_reg(state, CTX_ELR_EL3, ep->pc);
	write_ctx_reg(state, CTX_SPSR_EL3, ep->spsr);

	/*
	 * Store the X0-X7 value from the entrypoint into the context
	 * Use memcpy as we are in control of the layout of the structures
	 */
	gp_regs = get_gpregs_ctx(ctx);
	memcpy(gp_regs, (void *)&ep->args, sizeof(aapcs64_params_t));
}

/*******************************************************************************
 * Context management library initialization routine. This library is used by
 * runtime services to share pointers to 'cpu_context' structures for secure
 * non-secure and realm states. Management of the structures and their associated
 * memory is not done by the context management library e.g. the PSCI service
 * manages the cpu context used for entry from and exit to the non-secure state.
 * The Secure payload dispatcher service manages the context(s) corresponding to
 * the secure state. It also uses this library to get access to the non-secure
 * state cpu context pointers.
 * Lastly, this library provides the API to make SP_EL3 point to the cpu context
 * which will be used for programming an entry into a lower EL. The same context
 * will be used to save state upon exception entry from that EL.
 ******************************************************************************/
void __init cm_init(void)
{
	/*
	 * The context management library has only global data to initialize, but
	 * that will be done when the BSS is zeroed out.
	 */
}

/*******************************************************************************
 * This is the high-level function used to initialize the cpu_context 'ctx' for
 * first use. It performs initializations that are common to all security states
 * and initializations specific to the security state specified in 'ep'
 ******************************************************************************/
void cm_setup_context(cpu_context_t *ctx, const entry_point_info_t *ep)
{
	unsigned int security_state;

	assert(ctx != NULL);

	/*
	 * Perform initializations that are common
	 * to all security states
	 */
	setup_context_common(ctx, ep);

	security_state = GET_SECURITY_STATE(ep->h.attr);

	/* Perform security state specific initializations */
	switch (security_state) {
	case SECURE:
		setup_secure_context(ctx, ep);
		break;
#if ENABLE_RME
	case REALM:
		setup_realm_context(ctx, ep);
		break;
#endif
	case NON_SECURE:
		setup_ns_context(ctx, ep);
		break;
	default:
		ERROR("Invalid security state\n");
		panic();
		break;
	}
}

/*******************************************************************************
 * Enable architecture extensions on first entry to Non-secure world.
 * When EL2 is implemented but unused `el2_unused` is non-zero, otherwise
 * it is zero. This function updates some registers in-place and its contents
 * are being prepared to be moved to cm_manage_extensions_el3 and
 * cm_manage_extensions_nonsecure.
 ******************************************************************************/
static void manage_extensions_nonsecure_mixed(bool el2_unused, cpu_context_t *ctx)
{
#if IMAGE_BL31
	if (is_feat_amu_supported()) {
		amu_enable(el2_unused, ctx);
	}
#endif /* IMAGE_BL31 */
}

/*******************************************************************************
 * Enable architecture extensions for EL3 execution. This function only updates
 * registers in-place which are expected to either never change or be
 * overwritten by el3_exit.
 ******************************************************************************/
#if IMAGE_BL31
void cm_manage_extensions_el3(void)
{
	if (is_feat_spe_supported()) {
		spe_init_el3();
	}

	if (is_feat_sme_supported()) {
		sme_init_el3();
	}

	if (is_feat_mpam_supported()) {
		mpam_init_el3();
	}

	if (is_feat_trbe_supported()) {
		trbe_init_el3();
	}

	if (is_feat_brbe_supported()) {
		brbe_init_el3();
	}

	if (is_feat_trf_supported()) {
		trf_init_el3();
	}

	pmuv3_init_el3();
}
#endif /* IMAGE_BL31 */

/*******************************************************************************
 * Enable architecture extensions on first entry to Non-secure world.
 ******************************************************************************/
static void manage_extensions_nonsecure(cpu_context_t *ctx)
{
#if IMAGE_BL31
	/* Enable SVE and FPU/SIMD */
	if (is_feat_sve_supported()) {
		sve_enable(ctx);
	}

	if (is_feat_sme_supported()) {
		sme_enable(ctx);
	}

	if (is_feat_sys_reg_trace_supported()) {
		sys_reg_trace_enable(ctx);
	}

	pmuv3_enable(ctx);
#endif /* IMAGE_BL31 */
}

/*******************************************************************************
 * Enable architecture extensions in-place at EL2 on first entry to Non-secure
 * world when EL2 is empty and unused.
 ******************************************************************************/
static void manage_extensions_nonsecure_el2_unused(void)
{
#if IMAGE_BL31
	if (is_feat_spe_supported()) {
		spe_init_el2_unused();
	}

	if (is_feat_mpam_supported()) {
		mpam_init_el2_unused();
	}

	if (is_feat_trbe_supported()) {
		trbe_init_el2_unused();
	}

	if (is_feat_sys_reg_trace_supported()) {
		sys_reg_trace_init_el2_unused();
	}

	if (is_feat_trf_supported()) {
		trf_init_el2_unused();
	}

	pmuv3_init_el2_unused();

	if (is_feat_sve_supported()) {
		sve_init_el2_unused();
	}

	if (is_feat_sme_supported()) {
		sme_init_el2_unused();
	}
#endif /* IMAGE_BL31 */
}

/*******************************************************************************
 * Enable architecture extensions on first entry to Secure world.
 ******************************************************************************/
static void manage_extensions_secure(cpu_context_t *ctx)
{
#if IMAGE_BL31
	if (is_feat_sve_supported()) {
		if (ENABLE_SVE_FOR_SWD) {
		/*
		 * Enable SVE and FPU in secure context, secure manager must
		 * ensure that the SVE and FPU register contexts are properly
		 * managed.
		 */
			sve_enable(ctx);
		} else {
		/*
		 * Disable SVE and FPU in secure context so non-secure world
		 * can safely use them.
		 */
			sve_disable(ctx);
		}
	}

	if (is_feat_sme_supported()) {
		if (ENABLE_SME_FOR_SWD) {
		/*
		 * Enable SME, SVE, FPU/SIMD in secure context, secure manager
		 * must ensure SME, SVE, and FPU/SIMD context properly managed.
		 */
			sme_init_el3();
			sme_enable(ctx);
		} else {
		/*
		 * Disable SME, SVE, FPU/SIMD in secure context so non-secure
		 * world can safely use the associated registers.
		 */
			sme_disable(ctx);
		}
	}
#endif /* IMAGE_BL31 */
}

/*******************************************************************************
 * The following function initializes the cpu_context for a CPU specified by
 * its `cpu_idx` for first use, and sets the initial entrypoint state as
 * specified by the entry_point_info structure.
 ******************************************************************************/
void cm_init_context_by_index(unsigned int cpu_idx,
			      const entry_point_info_t *ep)
{
	cpu_context_t *ctx;
	ctx = cm_get_context_by_index(cpu_idx, GET_SECURITY_STATE(ep->h.attr));
	cm_setup_context(ctx, ep);
}

/*******************************************************************************
 * The following function initializes the cpu_context for the current CPU
 * for first use, and sets the initial entrypoint state as specified by the
 * entry_point_info structure.
 ******************************************************************************/
void cm_init_my_context(const entry_point_info_t *ep)
{
	cpu_context_t *ctx;
	ctx = cm_get_context(GET_SECURITY_STATE(ep->h.attr));
	cm_setup_context(ctx, ep);
}

/*******************************************************************************
 * Prepare the CPU system registers for first entry into realm, secure, or
 * normal world.
 *
 * If execution is requested to EL2 or hyp mode, SCTLR_EL2 is initialized
 * If execution is requested to non-secure EL1 or svc mode, and the CPU supports
 * EL2 then EL2 is disabled by configuring all necessary EL2 registers.
 * For all entries, the EL1 registers are initialized from the cpu_context
 ******************************************************************************/
void cm_prepare_el3_exit(uint32_t security_state)
{
	u_register_t sctlr_elx, scr_el3, mdcr_el2;
	cpu_context_t *ctx = cm_get_context(security_state);
	bool el2_unused = false;
	uint64_t hcr_el2 = 0U;

	assert(ctx != NULL);

	if (security_state == NON_SECURE) {
		uint64_t el2_implemented = el_implemented(2);

		scr_el3 = read_ctx_reg(get_el3state_ctx(ctx),
						 CTX_SCR_EL3);

		if (((scr_el3 & SCR_HCE_BIT) != 0U)
			|| (el2_implemented != EL_IMPL_NONE)) {
			/*
			 * If context is not being used for EL2, initialize
			 * HCRX_EL2 with its init value here.
			 */
			if (is_feat_hcx_supported()) {
				write_hcrx_el2(HCRX_EL2_INIT_VAL);
			}
		}

		if ((scr_el3 & SCR_HCE_BIT) != 0U) {
			/* Use SCTLR_EL1.EE value to initialise sctlr_el2 */
			sctlr_elx = read_ctx_reg(get_el1_sysregs_ctx(ctx),
							   CTX_SCTLR_EL1);
			sctlr_elx &= SCTLR_EE_BIT;
			sctlr_elx |= SCTLR_EL2_RES1;
#if ERRATA_A75_764081
			/*
			 * If workaround of errata 764081 for Cortex-A75 is used
			 * then set SCTLR_EL2.IESB to enable Implicit Error
			 * Synchronization Barrier.
			 */
			sctlr_elx |= SCTLR_IESB_BIT;
#endif
			write_sctlr_el2(sctlr_elx);
		} else if (el2_implemented != EL_IMPL_NONE) {
			el2_unused = true;

			/*
			 * EL2 present but unused, need to disable safely.
			 * SCTLR_EL2 can be ignored in this case.
			 *
			 * Set EL2 register width appropriately: Set HCR_EL2
			 * field to match SCR_EL3.RW.
			 */
			if ((scr_el3 & SCR_RW_BIT) != 0U)
				hcr_el2 |= HCR_RW_BIT;

			/*
			 * For Armv8.3 pointer authentication feature, disable
			 * traps to EL2 when accessing key registers or using
			 * pointer authentication instructions from lower ELs.
			 */
			hcr_el2 |= (HCR_API_BIT | HCR_APK_BIT);

			write_hcr_el2(hcr_el2);

			/*
			 * Initialise CPTR_EL2 setting all fields rather than
			 * relying on the hw. All fields have architecturally
			 * UNKNOWN reset values.
			 */
			write_cptr_el2(CPTR_EL2_RESET_VAL);

			/*
			 * Initialise CNTHCTL_EL2. All fields are
			 * architecturally UNKNOWN on reset and are set to zero
			 * except for field(s) listed below.
			 *
			 * CNTHCTL_EL2.EL1PTEN: Set to one to disable traps to
			 *  Hyp mode of Non-secure EL0 and EL1 accesses to the
			 *  physical timer registers.
			 *
			 * CNTHCTL_EL2.EL1PCTEN: Set to one to disable traps to
			 *  Hyp mode of  Non-secure EL0 and EL1 accesses to the
			 *  physical counter registers.
			 */
			write_cnthctl_el2(CNTHCTL_RESET_VAL |
						EL1PCEN_BIT | EL1PCTEN_BIT);

			/*
			 * Initialise CNTVOFF_EL2 to zero as it resets to an
			 * architecturally UNKNOWN value.
			 */
			write_cntvoff_el2(0);

			/*
			 * Set VPIDR_EL2 and VMPIDR_EL2 to match MIDR_EL1 and
			 * MPIDR_EL1 respectively.
			 */
			write_vpidr_el2(read_midr_el1());
			write_vmpidr_el2(read_mpidr_el1());

			/*
			 * Initialise VTTBR_EL2. All fields are architecturally
			 * UNKNOWN on reset.
			 *
			 * VTTBR_EL2.VMID: Set to zero. Even though EL1&0 stage
			 *  2 address translation is disabled, cache maintenance
			 *  operations depend on the VMID.
			 *
			 * VTTBR_EL2.BADDR: Set to zero as EL1&0 stage 2 address
			 *  translation is disabled.
			 */
			write_vttbr_el2(VTTBR_RESET_VAL &
				~((VTTBR_VMID_MASK << VTTBR_VMID_SHIFT)
				| (VTTBR_BADDR_MASK << VTTBR_BADDR_SHIFT)));

			/*
			 * Initialise MDCR_EL2, setting all fields rather than
			 * relying on hw. Some fields are architecturally
			 * UNKNOWN on reset.
			 *
			 * MDCR_EL2.TDRA: Set to zero so that Non-secure EL0 and
			 *  EL1 System register accesses to the Debug ROM
			 *  registers are not trapped to EL2.
			 *
			 * MDCR_EL2.TDOSA: Set to zero so that Non-secure EL1
			 *  System register accesses to the powerdown debug
			 *  registers are not trapped to EL2.
			 *
			 * MDCR_EL2.TDA: Set to zero so that System register
			 *  accesses to the debug registers do not trap to EL2.
			 *
			 * MDCR_EL2.TDE: Set to zero so that debug exceptions
			 *  are not routed to EL2.
			 */
			mdcr_el2 = ((MDCR_EL2_RESET_VAL) &
				   ~(MDCR_EL2_TDRA_BIT | MDCR_EL2_TDOSA_BIT |
				     MDCR_EL2_TDA_BIT | MDCR_EL2_TDE_BIT));

			write_mdcr_el2(mdcr_el2);

			/*
			 * Initialise HSTR_EL2. All fields are architecturally
			 * UNKNOWN on reset.
			 *
			 * HSTR_EL2.T<n>: Set all these fields to zero so that
			 *  Non-secure EL0 or EL1 accesses to System registers
			 *  do not trap to EL2.
			 */
			write_hstr_el2(HSTR_EL2_RESET_VAL & ~(HSTR_EL2_T_MASK));
			/*
			 * Initialise CNTHP_CTL_EL2. All fields are
			 * architecturally UNKNOWN on reset.
			 *
			 * CNTHP_CTL_EL2:ENABLE: Set to zero to disable the EL2
			 *  physical timer and prevent timer interrupts.
			 */
			write_cnthp_ctl_el2(CNTHP_CTL_RESET_VAL &
						~(CNTHP_CTL_ENABLE_BIT));

			manage_extensions_nonsecure_el2_unused();
		}
		manage_extensions_nonsecure_mixed(el2_unused, ctx);
	}

	cm_el1_sysregs_context_restore(security_state);
	cm_set_next_eret_context(security_state);
}

#if CTX_INCLUDE_EL2_REGS

static void el2_sysregs_context_save_fgt(el2_sysregs_t *ctx)
{
	write_ctx_reg(ctx, CTX_HDFGRTR_EL2, read_hdfgrtr_el2());
	if (is_feat_amu_supported()) {
		write_ctx_reg(ctx, CTX_HAFGRTR_EL2, read_hafgrtr_el2());
	}
	write_ctx_reg(ctx, CTX_HDFGWTR_EL2, read_hdfgwtr_el2());
	write_ctx_reg(ctx, CTX_HFGITR_EL2, read_hfgitr_el2());
	write_ctx_reg(ctx, CTX_HFGRTR_EL2, read_hfgrtr_el2());
	write_ctx_reg(ctx, CTX_HFGWTR_EL2, read_hfgwtr_el2());
}

static void el2_sysregs_context_restore_fgt(el2_sysregs_t *ctx)
{
	write_hdfgrtr_el2(read_ctx_reg(ctx, CTX_HDFGRTR_EL2));
	if (is_feat_amu_supported()) {
		write_hafgrtr_el2(read_ctx_reg(ctx, CTX_HAFGRTR_EL2));
	}
	write_hdfgwtr_el2(read_ctx_reg(ctx, CTX_HDFGWTR_EL2));
	write_hfgitr_el2(read_ctx_reg(ctx, CTX_HFGITR_EL2));
	write_hfgrtr_el2(read_ctx_reg(ctx, CTX_HFGRTR_EL2));
	write_hfgwtr_el2(read_ctx_reg(ctx, CTX_HFGWTR_EL2));
}

static void el2_sysregs_context_save_mpam(el2_sysregs_t *ctx)
{
	u_register_t mpam_idr = read_mpamidr_el1();

	write_ctx_reg(ctx, CTX_MPAM2_EL2, read_mpam2_el2());

	/*
	 * The context registers that we intend to save would be part of the
	 * PE's system register frame only if MPAMIDR_EL1.HAS_HCR == 1.
	 */
	if ((mpam_idr & MPAMIDR_HAS_HCR_BIT) == 0U) {
		return;
	}

	/*
	 * MPAMHCR_EL2, MPAMVPMV_EL2 and MPAMVPM0_EL2 are always present if
	 * MPAMIDR_HAS_HCR_BIT == 1.
	 */
	write_ctx_reg(ctx, CTX_MPAMHCR_EL2, read_mpamhcr_el2());
	write_ctx_reg(ctx, CTX_MPAMVPM0_EL2, read_mpamvpm0_el2());
	write_ctx_reg(ctx, CTX_MPAMVPMV_EL2, read_mpamvpmv_el2());

	/*
	 * The number of MPAMVPM registers is implementation defined, their
	 * number is stored in the MPAMIDR_EL1 register.
	 */
	switch ((mpam_idr >> MPAMIDR_EL1_VPMR_MAX_SHIFT) & MPAMIDR_EL1_VPMR_MAX_MASK) {
	case 7:
		write_ctx_reg(ctx, CTX_MPAMVPM7_EL2, read_mpamvpm7_el2());
		__fallthrough;
	case 6:
		write_ctx_reg(ctx, CTX_MPAMVPM6_EL2, read_mpamvpm6_el2());
		__fallthrough;
	case 5:
		write_ctx_reg(ctx, CTX_MPAMVPM5_EL2, read_mpamvpm5_el2());
		__fallthrough;
	case 4:
		write_ctx_reg(ctx, CTX_MPAMVPM4_EL2, read_mpamvpm4_el2());
		__fallthrough;
	case 3:
		write_ctx_reg(ctx, CTX_MPAMVPM3_EL2, read_mpamvpm3_el2());
		__fallthrough;
	case 2:
		write_ctx_reg(ctx, CTX_MPAMVPM2_EL2, read_mpamvpm2_el2());
		__fallthrough;
	case 1:
		write_ctx_reg(ctx, CTX_MPAMVPM1_EL2, read_mpamvpm1_el2());
		break;
	}
}

static void el2_sysregs_context_restore_mpam(el2_sysregs_t *ctx)
{
	u_register_t mpam_idr = read_mpamidr_el1();

	write_mpam2_el2(read_ctx_reg(ctx, CTX_MPAM2_EL2));

	if ((mpam_idr & MPAMIDR_HAS_HCR_BIT) == 0U) {
		return;
	}

	write_mpamhcr_el2(read_ctx_reg(ctx, CTX_MPAMHCR_EL2));
	write_mpamvpm0_el2(read_ctx_reg(ctx, CTX_MPAMVPM0_EL2));
	write_mpamvpmv_el2(read_ctx_reg(ctx, CTX_MPAMVPMV_EL2));

	switch ((mpam_idr >> MPAMIDR_EL1_VPMR_MAX_SHIFT) & MPAMIDR_EL1_VPMR_MAX_MASK) {
	case 7:
		write_mpamvpm7_el2(read_ctx_reg(ctx, CTX_MPAMVPM7_EL2));
		__fallthrough;
	case 6:
		write_mpamvpm6_el2(read_ctx_reg(ctx, CTX_MPAMVPM6_EL2));
		__fallthrough;
	case 5:
		write_mpamvpm5_el2(read_ctx_reg(ctx, CTX_MPAMVPM5_EL2));
		__fallthrough;
	case 4:
		write_mpamvpm4_el2(read_ctx_reg(ctx, CTX_MPAMVPM4_EL2));
		__fallthrough;
	case 3:
		write_mpamvpm3_el2(read_ctx_reg(ctx, CTX_MPAMVPM3_EL2));
		__fallthrough;
	case 2:
		write_mpamvpm2_el2(read_ctx_reg(ctx, CTX_MPAMVPM2_EL2));
		__fallthrough;
	case 1:
		write_mpamvpm1_el2(read_ctx_reg(ctx, CTX_MPAMVPM1_EL2));
		break;
	}
}

/*******************************************************************************
 * Save EL2 sysreg context
 ******************************************************************************/
void cm_el2_sysregs_context_save(uint32_t security_state)
{
	u_register_t scr_el3 = read_scr();

	/*
	 * Always save the non-secure and realm EL2 context, only save the
	 * S-EL2 context if S-EL2 is enabled.
	 */
	if ((security_state != SECURE) ||
	    ((security_state == SECURE) && ((scr_el3 & SCR_EEL2_BIT) != 0U))) {
		cpu_context_t *ctx;
		el2_sysregs_t *el2_sysregs_ctx;

		ctx = cm_get_context(security_state);
		assert(ctx != NULL);

		el2_sysregs_ctx = get_el2_sysregs_ctx(ctx);

		el2_sysregs_context_save_common(el2_sysregs_ctx);
#if CTX_INCLUDE_MTE_REGS
		el2_sysregs_context_save_mte(el2_sysregs_ctx);
#endif
		if (is_feat_mpam_supported()) {
			el2_sysregs_context_save_mpam(el2_sysregs_ctx);
		}

		if (is_feat_fgt_supported()) {
			el2_sysregs_context_save_fgt(el2_sysregs_ctx);
		}

		if (is_feat_ecv_v2_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_CNTPOFF_EL2,
				      read_cntpoff_el2());
		}

		if (is_feat_vhe_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_CONTEXTIDR_EL2,
				      read_contextidr_el2());
			write_ctx_reg(el2_sysregs_ctx, CTX_TTBR1_EL2,
				      read_ttbr1_el2());
		}

		if (is_feat_ras_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_VDISR_EL2,
				      read_vdisr_el2());
			write_ctx_reg(el2_sysregs_ctx, CTX_VSESR_EL2,
				      read_vsesr_el2());
		}

		if (is_feat_nv2_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_VNCR_EL2,
				      read_vncr_el2());
		}

		if (is_feat_trf_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_TRFCR_EL2, read_trfcr_el2());
		}

		if (is_feat_csv2_2_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_SCXTNUM_EL2,
				      read_scxtnum_el2());
		}

		if (is_feat_hcx_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_HCRX_EL2, read_hcrx_el2());
		}
		if (is_feat_tcr2_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_TCR2_EL2, read_tcr2_el2());
		}
		if (is_feat_sxpie_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_PIRE0_EL2, read_pire0_el2());
			write_ctx_reg(el2_sysregs_ctx, CTX_PIR_EL2, read_pir_el2());
		}
		if (is_feat_s2pie_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_S2PIR_EL2, read_s2pir_el2());
		}
		if (is_feat_sxpoe_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_POR_EL2, read_por_el2());
		}
		if (is_feat_gcs_supported()) {
			write_ctx_reg(el2_sysregs_ctx, CTX_GCSPR_EL2, read_gcspr_el2());
			write_ctx_reg(el2_sysregs_ctx, CTX_GCSCR_EL2, read_gcscr_el2());
		}
	}
}

/*******************************************************************************
 * Restore EL2 sysreg context
 ******************************************************************************/
void cm_el2_sysregs_context_restore(uint32_t security_state)
{
	u_register_t scr_el3 = read_scr();

	/*
	 * Always restore the non-secure and realm EL2 context, only restore the
	 * S-EL2 context if S-EL2 is enabled.
	 */
	if ((security_state != SECURE) ||
	    ((security_state == SECURE) && ((scr_el3 & SCR_EEL2_BIT) != 0U))) {
		cpu_context_t *ctx;
		el2_sysregs_t *el2_sysregs_ctx;

		ctx = cm_get_context(security_state);
		assert(ctx != NULL);

		el2_sysregs_ctx = get_el2_sysregs_ctx(ctx);

		el2_sysregs_context_restore_common(el2_sysregs_ctx);
#if CTX_INCLUDE_MTE_REGS
		el2_sysregs_context_restore_mte(el2_sysregs_ctx);
#endif
		if (is_feat_mpam_supported()) {
			el2_sysregs_context_restore_mpam(el2_sysregs_ctx);
		}

		if (is_feat_fgt_supported()) {
			el2_sysregs_context_restore_fgt(el2_sysregs_ctx);
		}

		if (is_feat_ecv_v2_supported()) {
			write_cntpoff_el2(read_ctx_reg(el2_sysregs_ctx,
						       CTX_CNTPOFF_EL2));
		}

		if (is_feat_vhe_supported()) {
			write_contextidr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_CONTEXTIDR_EL2));
			write_ttbr1_el2(read_ctx_reg(el2_sysregs_ctx, CTX_TTBR1_EL2));
		}

		if (is_feat_ras_supported()) {
			write_vdisr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_VDISR_EL2));
			write_vsesr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_VSESR_EL2));
		}

		if (is_feat_nv2_supported()) {
			write_vncr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_VNCR_EL2));
		}
		if (is_feat_trf_supported()) {
			write_trfcr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_TRFCR_EL2));
		}

		if (is_feat_csv2_2_supported()) {
			write_scxtnum_el2(read_ctx_reg(el2_sysregs_ctx,
						       CTX_SCXTNUM_EL2));
		}

		if (is_feat_hcx_supported()) {
			write_hcrx_el2(read_ctx_reg(el2_sysregs_ctx, CTX_HCRX_EL2));
		}
		if (is_feat_tcr2_supported()) {
			write_tcr2_el2(read_ctx_reg(el2_sysregs_ctx, CTX_TCR2_EL2));
		}
		if (is_feat_sxpie_supported()) {
			write_pire0_el2(read_ctx_reg(el2_sysregs_ctx, CTX_PIRE0_EL2));
			write_pir_el2(read_ctx_reg(el2_sysregs_ctx, CTX_PIR_EL2));
		}
		if (is_feat_s2pie_supported()) {
			write_s2pir_el2(read_ctx_reg(el2_sysregs_ctx, CTX_S2PIR_EL2));
		}
		if (is_feat_sxpoe_supported()) {
			write_por_el2(read_ctx_reg(el2_sysregs_ctx, CTX_POR_EL2));
		}
		if (is_feat_gcs_supported()) {
			write_gcscr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_GCSCR_EL2));
			write_gcspr_el2(read_ctx_reg(el2_sysregs_ctx, CTX_GCSPR_EL2));
		}
	}
}
#endif /* CTX_INCLUDE_EL2_REGS */

/*******************************************************************************
 * This function is used to exit to Non-secure world. If CTX_INCLUDE_EL2_REGS
 * is enabled, it restores EL1 and EL2 sysreg contexts instead of directly
 * updating EL1 and EL2 registers. Otherwise, it calls the generic
 * cm_prepare_el3_exit function.
 ******************************************************************************/
void cm_prepare_el3_exit_ns(void)
{
#if CTX_INCLUDE_EL2_REGS
	cpu_context_t *ctx = cm_get_context(NON_SECURE);
	assert(ctx != NULL);

	/* Assert that EL2 is used. */
#if ENABLE_ASSERTIONS
	el3_state_t *state = get_el3state_ctx(ctx);
	u_register_t scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);
#endif
	assert(((scr_el3 & SCR_HCE_BIT) != 0UL) &&
			(el_implemented(2U) != EL_IMPL_NONE));

	/*
	 * Currently some extensions are configured using
	 * direct register updates. Therefore, do this here
	 * instead of when setting up context.
	 */
	manage_extensions_nonsecure_mixed(0, ctx);

	/*
	 * Set the NS bit to be able to access the ICC_SRE_EL2
	 * register when restoring context.
	 */
	write_scr_el3(read_scr_el3() | SCR_NS_BIT);

	/*
	 * Ensure the NS bit change is committed before the EL2/EL1
	 * state restoration.
	 */
	isb();

	/* Restore EL2 and EL1 sysreg contexts */
	cm_el2_sysregs_context_restore(NON_SECURE);
	cm_el1_sysregs_context_restore(NON_SECURE);
	cm_set_next_eret_context(NON_SECURE);
#else
	cm_prepare_el3_exit(NON_SECURE);
#endif /* CTX_INCLUDE_EL2_REGS */
}

/*******************************************************************************
 * The next four functions are used by runtime services to save and restore
 * EL1 context on the 'cpu_context' structure for the specified security
 * state.
 ******************************************************************************/
void cm_el1_sysregs_context_save(uint32_t security_state)
{
	cpu_context_t *ctx;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	el1_sysregs_context_save(get_el1_sysregs_ctx(ctx));

#if IMAGE_BL31
	if (security_state == SECURE)
		PUBLISH_EVENT(cm_exited_secure_world);
	else
		PUBLISH_EVENT(cm_exited_normal_world);
#endif
}

void cm_el1_sysregs_context_restore(uint32_t security_state)
{
	cpu_context_t *ctx;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	el1_sysregs_context_restore(get_el1_sysregs_ctx(ctx));

#if IMAGE_BL31
	if (security_state == SECURE)
		PUBLISH_EVENT(cm_entering_secure_world);
	else
		PUBLISH_EVENT(cm_entering_normal_world);
#endif
}

/*******************************************************************************
 * This function populates ELR_EL3 member of 'cpu_context' pertaining to the
 * given security state with the given entrypoint
 ******************************************************************************/
void cm_set_elr_el3(uint32_t security_state, uintptr_t entrypoint)
{
	cpu_context_t *ctx;
	el3_state_t *state;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	/* Populate EL3 state so that ERET jumps to the correct entry */
	state = get_el3state_ctx(ctx);
	write_ctx_reg(state, CTX_ELR_EL3, entrypoint);
}

/*******************************************************************************
 * This function populates ELR_EL3 and SPSR_EL3 members of 'cpu_context'
 * pertaining to the given security state
 ******************************************************************************/
void cm_set_elr_spsr_el3(uint32_t security_state,
			uintptr_t entrypoint, uint32_t spsr)
{
	cpu_context_t *ctx;
	el3_state_t *state;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	/* Populate EL3 state so that ERET jumps to the correct entry */
	state = get_el3state_ctx(ctx);
	write_ctx_reg(state, CTX_ELR_EL3, entrypoint);
	write_ctx_reg(state, CTX_SPSR_EL3, spsr);
}

/*******************************************************************************
 * This function updates a single bit in the SCR_EL3 member of the 'cpu_context'
 * pertaining to the given security state using the value and bit position
 * specified in the parameters. It preserves all other bits.
 ******************************************************************************/
void cm_write_scr_el3_bit(uint32_t security_state,
			  uint32_t bit_pos,
			  uint32_t value)
{
	cpu_context_t *ctx;
	el3_state_t *state;
	u_register_t scr_el3;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	/* Ensure that the bit position is a valid one */
	assert(((1UL << bit_pos) & SCR_VALID_BIT_MASK) != 0U);

	/* Ensure that the 'value' is only a bit wide */
	assert(value <= 1U);

	/*
	 * Get the SCR_EL3 value from the cpu context, clear the desired bit
	 * and set it to its new value.
	 */
	state = get_el3state_ctx(ctx);
	scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);
	scr_el3 &= ~(1UL << bit_pos);
	scr_el3 |= (u_register_t)value << bit_pos;
	write_ctx_reg(state, CTX_SCR_EL3, scr_el3);
}

/*******************************************************************************
 * This function retrieves SCR_EL3 member of 'cpu_context' pertaining to the
 * given security state.
 ******************************************************************************/
u_register_t cm_get_scr_el3(uint32_t security_state)
{
	cpu_context_t *ctx;
	el3_state_t *state;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	/* Populate EL3 state so that ERET jumps to the correct entry */
	state = get_el3state_ctx(ctx);
	return read_ctx_reg(state, CTX_SCR_EL3);
}

/*******************************************************************************
 * This function is used to program the context that's used for exception
 * return. This initializes the SP_EL3 to a pointer to a 'cpu_context' set for
 * the required security state
 ******************************************************************************/
void cm_set_next_eret_context(uint32_t security_state)
{
	cpu_context_t *ctx;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	cm_set_next_context(ctx);
}
