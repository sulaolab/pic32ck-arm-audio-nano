/*
 * dsp_bench.c - see dsp_bench.h.
 */
#include "app_config.h"

/*
 * This is measurement-only code, so the whole translation unit is behind
 * APP_ENABLE_DSP_BENCH. It compiles to nothing in the live audio
 * configurations.
 */

#if APP_ENABLE_DSP_BENCH

#include <string.h>

#include "board/board.h"
#include "drivers/console.h"

#include "dsp_bench.h"
#include "dsp_coeffs.h"
#include "dsp_cycles.h"
#include "dsp_dwt_diag.h"
#include "dsp_signal.h"
#include "dsp_tier2.h"
#include "dsp_vectors.h"

/* ------------------------------------------------------------------ */
/* Working set. Static, like everything else in this project.           */
/* ------------------------------------------------------------------ */
#define DSP_BENCH_BUF_WORDS (DSP_BENCH_MAX_CHANNELS * DSP_IIR_FRAME_STRIDE)

static float32_t s_in[DSP_BENCH_BUF_WORDS];
static float32_t s_out[DSP_BENCH_BUF_WORDS];
static dsp_iir_t s_filter;

/*
 * Somewhere for the output to go that the optimiser cannot argue away.
 * dsp_iir_process() writes through an external call into a file-scope
 * array, so it cannot be elided in any case, but folding the result into a
 * volatile sink removes the question entirely - and it is done OUTSIDE the
 * timed region so it costs the measurement nothing.
 */
static volatile uint32_t s_sink;

static bool s_ready;

static void print_milli(const char *label, uint32_t milli, const char *unit);
static uint32_t scaled_div(uint32_t a, uint32_t scale, uint32_t b);

/* ------------------------------------------------------------------ */
/* Cortex-M33 FPv5-SP-D16 instruction probe                            */
/* ------------------------------------------------------------------ */
/*
 * Public Arm and Microchip documentation specifies the instructions but not
 * the implementation's dependent latency/reciprocal throughput.  Keep the
 * streams in inline assembly so this probe measures exactly the stated
 * schedule, independently of compiler heuristics.  s0-s15 are caller-saved
 * under AAPCS32, and only s0-s9 are touched here.
 *
 * Every VFMA test executes 32 operations per loop.  The destination rotation
 * is the only difference: one chain exposes latency; 8 chains expose peak
 * issue throughput if the implementation has enough independent work.
 */
#if defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
#define DSP_FPU_PROBE_LOOPS 2048u
#define DSP_FPU_PROBE_OPS_PER_LOOP 32u
#define DSP_FPU_PROBE_REPEATS 8u

#define FMA1  "vfma.f32 s0, s8, s9\n\t"
#define FMA2  "vfma.f32 s0, s8, s9\n\t" "vfma.f32 s1, s8, s9\n\t"
#define FMA4  FMA2 "vfma.f32 s2, s8, s9\n\t" "vfma.f32 s3, s8, s9\n\t"
#define FMA8  FMA4 \
              "vfma.f32 s4, s8, s9\n\t" "vfma.f32 s5, s8, s9\n\t" \
              "vfma.f32 s6, s8, s9\n\t" "vfma.f32 s7, s8, s9\n\t"
#define MUL1  "vmul.f32 s0, s0, s8\n\t"
#define MUL8  "vmul.f32 s0, s0, s8\n\t" "vmul.f32 s1, s1, s8\n\t" \
              "vmul.f32 s2, s2, s8\n\t" "vmul.f32 s3, s3, s8\n\t" \
              "vmul.f32 s4, s4, s8\n\t" "vmul.f32 s5, s5, s8\n\t" \
              "vmul.f32 s6, s6, s8\n\t" "vmul.f32 s7, s7, s8\n\t"
#define ADD1  "vadd.f32 s0, s0, s9\n\t"
#define ADD8  "vadd.f32 s0, s0, s9\n\t" "vadd.f32 s1, s1, s9\n\t" \
              "vadd.f32 s2, s2, s9\n\t" "vadd.f32 s3, s3, s9\n\t" \
              "vadd.f32 s4, s4, s9\n\t" "vadd.f32 s5, s5, s9\n\t" \
              "vadd.f32 s6, s6, s9\n\t" "vadd.f32 s7, s7, s9\n\t"
#define MLA1  "vmla.f32 s0, s8, s9\n\t"
#define MLA8  "vmla.f32 s0, s8, s9\n\t" "vmla.f32 s1, s8, s9\n\t" \
              "vmla.f32 s2, s8, s9\n\t" "vmla.f32 s3, s8, s9\n\t" \
              "vmla.f32 s4, s8, s9\n\t" "vmla.f32 s5, s8, s9\n\t" \
              "vmla.f32 s6, s8, s9\n\t" "vmla.f32 s7, s8, s9\n\t"
#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define REP32(x) REP16(x) REP16(x)

static volatile uint32_t s_fpu_probe_sink;

static __attribute__((noinline)) void fpu_probe_empty(uint32_t loops)
{
    __asm volatile (
        ".p2align 2\n\t"
        "1:\n\t"
        "subs %0, %0, #1\n\t"
        "bne 1b\n\t"
        : "+r" (loops)
        :
        : "cc");
}

#define DEFINE_VFMA_PROBE(name, stream)                                      \
static __attribute__((noinline)) void name(uint32_t loops)                   \
{                                                                            \
    uint32_t sink;                                                           \
    const uint32_t initial = 0x3F000000u; /* 0.5f */                         \
    const uint32_t mul     = 0x3F800001u; /* 1.0f + one ULP */               \
    const uint32_t add     = 0x39000000u; /* small, remains finite */         \
    __asm volatile (                                                        \
        "vmov s0, %2\n\t"                                                   \
        "vmov.f32 s1, s0\n\t" "vmov.f32 s2, s0\n\t"                      \
        "vmov.f32 s3, s0\n\t" "vmov.f32 s4, s0\n\t"                      \
        "vmov.f32 s5, s0\n\t" "vmov.f32 s6, s0\n\t"                      \
        "vmov.f32 s7, s0\n\t"                                             \
        "vmov s8, %3\n\t" "vmov s9, %4\n\t"                              \
        ".p2align 2\n\t"                                                   \
        "1:\n\t" stream                                                    \
        "subs %0, %0, #1\n\t"                                              \
        "bne 1b\n\t"                                                       \
        "vmov %1, s0\n\t"                                                 \
        : "+r" (loops), "=r" (sink)                                        \
        : "r" (initial), "r" (mul), "r" (add)                              \
        : "cc", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",       \
          "s8", "s9");                                                     \
    s_fpu_probe_sink ^= sink;                                                \
}

DEFINE_VFMA_PROBE(fpu_probe_vfma_1, REP32(FMA1))
DEFINE_VFMA_PROBE(fpu_probe_vfma_2, REP16(FMA2))
DEFINE_VFMA_PROBE(fpu_probe_vfma_4, REP8(FMA4))
DEFINE_VFMA_PROBE(fpu_probe_vfma_8, REP4(FMA8))
DEFINE_VFMA_PROBE(fpu_probe_vmul_1, REP32(MUL1))
DEFINE_VFMA_PROBE(fpu_probe_vmul_8, REP4(MUL8))
DEFINE_VFMA_PROBE(fpu_probe_vadd_1, REP32(ADD1))
DEFINE_VFMA_PROBE(fpu_probe_vadd_8, REP4(ADD8))
DEFINE_VFMA_PROBE(fpu_probe_vmla_1, REP32(MLA1))
DEFINE_VFMA_PROBE(fpu_probe_vmla_8, REP4(MLA8))

#undef DEFINE_VFMA_PROBE
#undef REP32
#undef REP16
#undef REP8
#undef REP4
#undef REP2
#undef FMA8
#undef FMA4
#undef FMA2
#undef FMA1
#undef MUL8
#undef MUL1
#undef ADD8
#undef ADD1
#undef MLA8
#undef MLA1

typedef void (*fpu_probe_fn_t)(uint32_t loops);

static uint32_t fpu_probe_min_cycles(fpu_probe_fn_t fn)
{
    uint32_t best = 0xFFFFFFFFu;

    fn(8u); /* instruction-cache and branch-predictor warm-up */
    for (uint32_t i = 0u; i < DSP_FPU_PROBE_REPEATS; i++)
    {
        const uint32_t t0 = dsp_cycles_now();
        fn(DSP_FPU_PROBE_LOOPS);
        const uint32_t t1 = dsp_cycles_now();
        uint32_t d = dsp_cycles_delta(t0, t1);
        const uint32_t overhead = dsp_cycles_overhead();
        d = (d > overhead) ? (d - overhead) : 0u;
        if (d < best)
        {
            best = d;
        }
    }
    return best;
}

static void fpu_probe_print(const char *name, const char *unit,
                            fpu_probe_fn_t fn)
{
    const uint32_t cycles = fpu_probe_min_cycles(fn);
    const uint32_t ops = DSP_FPU_PROBE_LOOPS * DSP_FPU_PROBE_OPS_PER_LOOP;
    console_printf("  %s: %u cycles, ", name, cycles);
    print_milli("", scaled_div(cycles, 1000u, ops), unit);
}
#endif

void dsp_bench_run_fpu_probe(void)
{
#if defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
    if (!s_ready || (dsp_cycles_source() == DSP_CYCLES_SRC_NONE))
    {
        console_writeln("FPU probe: refused - DWT/SysTick timing unavailable");
        return;
    }

    console_writeln("--- M33 FPv5-SP-D16 arithmetic probe ---------");
    console_printf("  stream: %u loops x %u operations, best of %u\n",
                   (uint32_t)DSP_FPU_PROBE_LOOPS,
                   (uint32_t)DSP_FPU_PROBE_OPS_PER_LOOP,
                   (uint32_t)DSP_FPU_PROBE_REPEATS);
    const uint32_t empty = fpu_probe_min_cycles(fpu_probe_empty);
    console_printf("  empty loop         %u cycles, ", empty);
    print_milli("", scaled_div(empty, 1000u, DSP_FPU_PROBE_LOOPS),
                "cycles/loop");
    fpu_probe_print("VFMA 1 dependent", "cycles/VFMA", fpu_probe_vfma_1);
    fpu_probe_print("VFMA 2 independent", "cycles/VFMA", fpu_probe_vfma_2);
    fpu_probe_print("VFMA 4 independent", "cycles/VFMA", fpu_probe_vfma_4);
    fpu_probe_print("VFMA 8 independent", "cycles/VFMA", fpu_probe_vfma_8);
    fpu_probe_print("VMUL 1 dependent", "cycles/VMUL", fpu_probe_vmul_1);
    fpu_probe_print("VMUL 8 independent", "cycles/VMUL", fpu_probe_vmul_8);
    fpu_probe_print("VADD 1 dependent", "cycles/VADD", fpu_probe_vadd_1);
    fpu_probe_print("VADD 8 independent", "cycles/VADD", fpu_probe_vadd_8);
    fpu_probe_print("VMLA 1 dependent", "cycles/VMLA", fpu_probe_vmla_1);
    fpu_probe_print("VMLA 8 independent", "cycles/VMLA", fpu_probe_vmla_8);
    console_writeln("  interpretation: 1-chain exposes dependency latency; the");
    console_writeln("  best wider stream approaches reciprocal issue throughput.");
    console_writeln("------------------------------------------------");
#else
    console_writeln("FPU probe: unavailable - target is not hard-float Arm");
#endif
}

/* ------------------------------------------------------------------ */
/* Printing without a float formatter                                  */
/* ------------------------------------------------------------------ */
/*
 * console_printf() has no %f, on purpose: this project links no libc
 * float formatting. Fractions are therefore printed as a scaled integer
 * pair, which is exact and cheap. All the ratios below are reported in
 * thousandths.
 */
static void print_milli(const char *label, uint32_t milli, const char *unit)
{
    console_printf("%s%u.%03u", label, milli / 1000u, milli % 1000u);
    if (unit[0] != '\0')
    {
        console_printf(" %s", unit);
    }
    console_write("\r\n");
}

/* (a * scale) / b with a 64-bit intermediate, so a 120 MHz cycle count
 * scaled by 1e6 cannot overflow on the way. */
static uint32_t scaled_div(uint32_t a, uint32_t scale, uint32_t b)
{
    if (b == 0u)
    {
        return 0u;
    }
    return (uint32_t)(((uint64_t)a * (uint64_t)scale) / (uint64_t)b);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
bool dsp_bench_init(void)
{
    uint32_t bad = 0u;

    s_ready = false;

    const char *bad_bank = "?";

    if (!dsp_coeffs_self_check_all(&bad, &bad_bank))
    {
        console_printf("DSP bench: coefficient %u of bank %s does not hold its "
                       "intended binary32 value - the generated table and the "
                       "compiled constant disagree\n", bad, bad_bank);
        return false;
    }

    if (!dsp_cycles_init())
    {
        console_writeln("DSP bench: no usable cycle counter (neither DWT CYCCNT "
                        "nor SysTick advanced). Correctness tests still run; "
                        "timings will report as unavailable.");
        /* Not fatal: the known-answer test does not need a clock. */
    }

    s_ready = true;
    return true;
}

void dsp_bench_print_build_info(void)
{
    console_writeln("--- CMSIS-DSP DF2T f32 isolated bench -------");
#if defined(DSP_USE_M33_DF2T_OPT) && (DSP_USE_M33_DF2T_OPT != 0)
#if defined(DSP_M33_NO_FMA_CONTRACT) && (DSP_M33_NO_FMA_CONTRACT != 0)
    console_printf(" kernel      : arm_biquad_cascade_df2T_f32() [M33 3-stage fusion, fp-contract=off]\n");
#else
    console_printf(" kernel      : arm_biquad_cascade_df2T_f32() [M33 3-stage fusion candidate]\n");
#endif
#elif defined(DSP_M33_NO_FMA_CONTRACT) && (DSP_M33_NO_FMA_CONTRACT != 0)
    console_printf(" kernel      : arm_biquad_cascade_df2T_f32() [CMSIS C, fp-contract=off]\n");
#else
    console_printf(" kernel      : arm_biquad_cascade_df2T_f32() [CMSIS-DSP vendor baseline]\n");
#endif
    console_printf(" CMSIS-DSP   : 1.17.0, vendored in src/dsp/cmsis_dsp\n");

#if (DSP_BENCH_OPT_TAG != 0)
    console_printf(" optimisation: -O%u\n", (uint32_t)DSP_BENCH_OPT_TAG);
#elif defined(__OPTIMIZE__)
    console_printf(" optimisation: on, level not declared by the build\n");
#else
    console_printf(" optimisation: -O0  <-- bring-up build, NOT a benchmark\n");
#endif

#if defined(DSP_USE_M33_DF2T_OPT) && (DSP_USE_M33_DF2T_OPT != 0)
#if defined(DSP_M33_DF2T_UNROLL)
    console_printf(" LOOPUNROLL  : project %u-way unrolled stage-group loops\n",
                   (uint32_t)DSP_M33_DF2T_UNROLL);
#else
    console_printf(" LOOPUNROLL  : project 16-way unrolled stage-group loops\n");
#endif
#elif defined(ARM_MATH_LOOPUNROLL)
    console_printf(" LOOPUNROLL  : yes (CMSIS 16-way unrolled inner loop)\n");
#else
    console_printf(" LOOPUNROLL  : no  (plain 1-sample inner loop)\n");
#endif

#if defined(__ARM_FP) && (__ARM_FP != 0)
    console_printf(" FPU         : __ARM_FP=%u", (uint32_t)__ARM_FP);
#if defined(__ARM_PCS_VFP)
    console_write(", hard-float ABI");
#endif
    console_write("\n");
#else
    console_printf(" FPU         : NONE - float32 is being emulated, any "
                   "measurement here is meaningless\n");
#endif

#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP != 0)
    console_printf(" DSP ext     : yes (only matters for q15/q31, not this "
                   "float32 kernel)\n");
#endif

    console_printf(" geometry    : max %u ch x %u frames x %u stages\n",
                   (uint32_t)DSP_BENCH_MAX_CHANNELS,
                   (uint32_t)DSP_BENCH_MAX_FRAMES,
                   (uint32_t)DSP_BENCH_MAX_STAGES);
    console_printf(" cycle source: %s\n", dsp_cycles_source_name());
    console_printf(" instr. cost : %u cycles per timestamp pair (subtracted)\n",
                   dsp_cycles_overhead());
    console_printf(" transport   : NOT connected - kernel only. The result is a "
                   "floor, not a live-audio budget.\n");
    console_writeln("---------------------------------------------");
}

/* ------------------------------------------------------------------ */
/* Known-answer test                                                   */
/* ------------------------------------------------------------------ */
static void load_kat_input(void)
{
    for (uint32_t c = 0u; c < DSP_VECTORS_CHANNELS; c++)
    {
        for (uint32_t n = 0u; n < DSP_VECTORS_FRAMES; n++)
        {
            s_in[(c * DSP_IIR_FRAME_STRIDE) + n] =
                dsp_signal_bits_to_f32(dsp_vectors_input_bits[c][n]);
        }
    }
}

static float32_t f32_abs(float32_t v)
{
    return (v < 0.0f) ? -v : v;
}

/*
 * The two coefficient banks, described in one place so nothing downstream
 * has to remember which tolerance or which expected block goes with which
 * cascade length.
 */
typedef struct
{
    const char      *name;
    const float32_t *coeffs;
    uint32_t         coeff_stages;   /* how long the bank really is       */
    uint32_t         stages;         /* what the KAT runs                 */
    const uint32_t (*expected)[DSP_VECTORS_FRAMES];
    float32_t        tolerance;
    uint32_t         warmup_blocks;  /* whole blocks before the compared one */
} kat_bank_t;

static const kat_bank_t s_bank_lp = {
    "bw12_lp", dsp_coeffs_bw12_lp, DSP_COEFFS_BW12_LP_STAGES,
    DSP_VECTORS_STAGES, dsp_vectors_expected_bits, DSP_VECTORS_TOLERANCE, 0u,
};

static const kat_bank_t s_bank_ap = {
    "ap84", dsp_coeffs_ap84, DSP_COEFFS_AP_STAGES,
    DSP_VECTORS_AP_STAGES, dsp_vectors_ap_expected_bits,
    DSP_VECTORS_AP_TOLERANCE, DSP_VECTORS_AP_WARMUP_BLOCKS,
};

/*
 * Runs the vectors once and returns the worst absolute residual against the
 * double-precision reference, plus where it was. Returns false only if the
 * geometry could not be configured - a large residual is a result, not an
 * error, because the negative tests need it.
 */
static bool kat_once(const kat_bank_t *bank,
                     dsp_iir_fault_t fault,
                     float32_t *worst,
                     uint32_t *worst_ch,
                     uint32_t *worst_n)
{
    if (!dsp_iir_configure(&s_filter,
                           DSP_VECTORS_CHANNELS,
                           bank->stages,
                           bank->coeffs,
                           bank->coeff_stages,
                           fault))
    {
        return false;
    }

    load_kat_input();
    memset(s_out, 0, sizeof(s_out));

    /* DF2T carries state between blocks, so the reference is only the
     * reference for a run that starts from a defined state. For the short
     * bank that state is zero; the long bank needs whole blocks of warm-up
     * first, because 84 allpass sections delay the signal past one block and
     * the first block out of zero state is nearly silence. The generator
     * computed the expected block the same way.
     *
     * Warm-up runs in place on s_out and then s_in is reloaded, so the
     * compared block sees exactly the KAT input with the warmed state. */
    dsp_iir_reset(&s_filter);
    for (uint32_t w = 0u; w < bank->warmup_blocks; w++)
    {
        dsp_iir_process(&s_filter, s_in, s_out, DSP_VECTORS_FRAMES);
    }
    dsp_iir_process(&s_filter, s_in, s_out, DSP_VECTORS_FRAMES);

    float32_t w  = 0.0f;
    uint32_t  wc = 0u;
    uint32_t  wn = 0u;

    for (uint32_t c = 0u; c < DSP_VECTORS_CHANNELS; c++)
    {
        for (uint32_t n = 0u; n < DSP_VECTORS_FRAMES; n++)
        {
            const float32_t got = s_out[(c * DSP_IIR_FRAME_STRIDE) + n];
            const float32_t ref =
                dsp_signal_bits_to_f32(bank->expected[c][n]);
            const float32_t e = f32_abs(got - ref);

            if (e > w)
            {
                w  = e;
                wc = c;
                wn = n;
            }
        }
    }

    *worst    = w;
    *worst_ch = wc;
    *worst_n  = wn;
    return true;
}

/* Errors are printed in units of 1e-9 so no float formatter is needed.
 * Saturated rather than wrapped: a wrapped error magnitude could read as a
 * small number, and "small" is the answer that matters here. */
static uint32_t err_to_nano(float32_t e)
{
    const float32_t scaled = e * 1.0e9f;

    if (scaled >= 4.0e9f)
    {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)scaled;
}

static bool self_test_bank(const kat_bank_t *bank)
{
    float32_t worst = 0.0f;
    uint32_t  wc = 0u, wn = 0u;
    bool      ok = true;

    /* The tolerance is printed from the macro, not written out again as a
     * literal: two copies of a threshold drift apart. */
    console_printf("KAT [%s]: %u ch x %u frames x %u stages, %u warm-up blocks,"
                   " tolerance %u e-9\n",
                   bank->name,
                   (uint32_t)DSP_VECTORS_CHANNELS,
                   (uint32_t)DSP_VECTORS_FRAMES,
                   bank->stages,
                   bank->warmup_blocks,
                   err_to_nano(bank->tolerance));

    if (!kat_once(bank, DSP_IIR_FAULT_NONE, &worst, &wc, &wn))
    {
        console_writeln("KAT: could not configure the reference geometry - FAIL");
        return false;
    }

    const bool clean_pass = (worst <= bank->tolerance);

    console_printf("  clean run          : max |err| = %u e-9 at ch %u sample %u"
                   " -> %s\n",
                   err_to_nano(worst), wc, wn, clean_pass ? "PASS" : "FAIL");
    ok = clean_pass;

    /*
     * Negative half. Each injected defect must push the residual past the
     * tolerance. If any of them passes, the tolerance is too loose and the
     * clean PASS above means nothing - so a "PASS" here is reported only
     * when every fault was caught.
     */
    for (uint32_t f = (uint32_t)DSP_IIR_FAULT_NONE + 1u;
         f < (uint32_t)DSP_IIR_FAULT_COUNT;
         f++)
    {
        const dsp_iir_fault_t fault = (dsp_iir_fault_t)f;

        if (!kat_once(bank, fault, &worst, &wc, &wn))
        {
            console_printf("  fault run          : could not inject '%s' - FAIL\n",
                           dsp_iir_fault_name(fault));
            ok = false;
            continue;
        }

        const bool caught = (worst > bank->tolerance);

        console_printf("  fault run          : max |err| = %u e-9 -> %s  [%s]\n",
                       err_to_nano(worst),
                       caught ? "caught, as required" : "NOT CAUGHT - FAIL",
                       dsp_iir_fault_name(fault));
        if (!caught)
        {
            ok = false;
        }
    }

    /* Leave the filter in a sane state for whatever runs next. */
    (void)dsp_iir_configure(&s_filter,
                            DSP_BENCH_DEFAULT_CHANNELS,
                            DSP_BENCH_DEFAULT_STAGES,
                            dsp_coeffs_bw12_lp,
                            DSP_COEFFS_BW12_LP_STAGES,
                            DSP_IIR_FAULT_NONE);

    console_printf("  verdict [%s]      : %s\n", bank->name, ok ? "PASS" : "FAIL");
    return ok;
}

bool dsp_bench_self_test(void)
{
    /*
     * Both banks, because they answer different questions. The 6-section
     * Butterworth is the arithmetic check at the default operating point; the
     * 84-section allpass is the check that the cascade the long sweep times is
     * the cascade that was intended. Timing a length that was never verified
     * would produce a number nobody can defend.
     */
    const bool lp_ok = self_test_bank(&s_bank_lp);
    const bool ap_ok = self_test_bank(&s_bank_ap);
    const bool t2_ok = dsp_bench_self_test_tier2();
    const bool ok    = lp_ok && ap_ok && t2_ok;

    console_printf("KAT verdict: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */
bool dsp_bench_run_bank(uint32_t channels,
                        uint32_t frames,
                        uint32_t stages,
                        uint32_t iterations,
                        dsp_bench_bank_t bank,
                        dsp_bench_result_t *r)
{
    if ((r == NULL) || !s_ready)
    {
        return false;
    }

    const kat_bank_t *const b =
        (bank == DSP_BENCH_BANK_AP84) ? &s_bank_ap : &s_bank_lp;

    memset(r, 0, sizeof(*r));
    r->channels   = channels;
    r->frames     = frames;
    r->stages     = stages;
    r->iterations = iterations;
    r->bank       = b->name;
    r->overhead   = dsp_cycles_overhead();

    if ((frames == 0u) || (frames > DSP_IIR_FRAME_STRIDE) || (iterations == 0u))
    {
        return false;
    }
    if (!dsp_iir_configure(&s_filter, channels, stages,
                           b->coeffs, b->coeff_stages, DSP_IIR_FAULT_NONE))
    {
        return false;
    }

    for (uint32_t c = 0u; c < channels; c++)
    {
        dsp_signal_fill_lcg(&s_in[c * DSP_IIR_FRAME_STRIDE], frames, c);
    }

    dsp_iir_reset(&s_filter);

    /*
     * Warm-up, untimed. Two jobs: bring the instruction cache and the
     * branch predictors to the state a steadily running filter would be
     * in, and let the DF2T state variables reach realistic magnitudes.
     * A first-ever call through cold flash is not the number anyone wants.
     *
     * The long allpass bank needs more than 4 blocks for the second job:
     * its group delay exceeds one block, so 4 blocks would time a cascade
     * whose state has not yet been reached by the signal. Its own warm-up
     * count is used, and never fewer than the 4 the short bank uses.
     */
    const uint32_t warmup =
        (b->warmup_blocks > 4u) ? b->warmup_blocks : 4u;

    for (uint32_t i = 0u; i < warmup; i++)
    {
        dsp_iir_process(&s_filter, s_in, s_out, frames);
    }

    if (dsp_cycles_source() == DSP_CYCLES_SRC_NONE)
    {
        r->valid = false;
        return true;    /* the run happened; only the timing is unavailable */
    }

    const uint32_t span  = dsp_cycles_span();
    const uint32_t limit = span / 4u;   /* stay well clear of ambiguity */
    uint32_t min = 0xFFFFFFFFu;
    uint32_t max = 0u;
    uint64_t sum = 0u;
    bool     fits = true;

    for (uint32_t i = 0u; i < iterations; i++)
    {
        const uint32_t t0 = dsp_cycles_now();
        dsp_iir_process(&s_filter, s_in, s_out, frames);
        const uint32_t t1 = dsp_cycles_now();

        uint32_t d = dsp_cycles_delta(t0, t1);

        if (d > limit)
        {
            /* Longer than a quarter of the counter range: on a 24-bit
             * down-counter that is indistinguishable from a wrap, so it is
             * refused instead of reported. */
            fits = false;
            break;
        }

        d = (d > r->overhead) ? (d - r->overhead) : 0u;

        if (d < min) { min = d; }
        if (d > max) { max = d; }
        sum += d;
    }

    /* Consume the output so no part of the chain can be argued away, and
     * do it outside the timed region. */
    uint32_t acc = 0u;
    for (uint32_t c = 0u; c < channels; c++)
    {
        acc ^= dsp_signal_f32_to_bits(s_out[c * DSP_IIR_FRAME_STRIDE]);
    }
    s_sink = acc;

    if (!fits)
    {
        r->valid = false;
        return true;
    }

    r->cycles_min  = min;
    r->cycles_max  = max;
    r->cycles_mean = (uint32_t)(sum / (uint64_t)iterations);
    r->valid       = true;
    return true;
}

bool dsp_bench_run(uint32_t channels,
                   uint32_t frames,
                   uint32_t stages,
                   uint32_t iterations,
                   dsp_bench_result_t *r)
{
    return dsp_bench_run_bank(channels, frames, stages, iterations,
                              DSP_BENCH_BANK_BW12_LP, r);
}

/*
 * N_max: how many sections per channel fit one block period, kernel only, at
 * the protocol's Tier 2 channel count.
 *
 *   N_max = block_cycles / (DSP_TIER2_DSP_CHANNELS * frames * C)
 *
 * where C = cycles_min / (channels * frames * stages) is the measured cost
 * per sample per section. Substituting and cancelling gives an exact integer
 * expression with no intermediate rounding:
 *
 *   N_max = block_cycles * channels * stages
 *           / (DSP_TIER2_DSP_CHANNELS * cycles_min)
 *
 * This figure folds clock rate into a block-budget estimate at the declared
 * channel and frame geometry.
 */
static uint32_t n_max_sections(const dsp_bench_result_t *r, uint32_t block_cycles)
{
    if (!r->valid || (r->cycles_min == 0u))
    {
        return 0u;
    }

    const uint64_t num = (uint64_t)block_cycles * r->channels * r->stages;
    const uint64_t den = (uint64_t)DSP_TIER2_DSP_CHANNELS * r->cycles_min;

    return (den == 0u) ? 0u : (uint32_t)(num / den);
}

void dsp_bench_print_result(const dsp_bench_result_t *r)
{
    console_printf("%u ch x %u frames x %u stages, %u iterations, bank %s\n",
                   r->channels, r->frames, r->stages, r->iterations,
                   (r->bank != NULL) ? r->bank : "?");

    if (!r->valid)
    {
        console_writeln("  timing unavailable - see the cycle source line in "
                        "the build info. The filter ran; nothing was measured.");
        return;
    }

    const uint32_t cpu     = board_cpu_clock_hz();
    const uint32_t samples = r->channels * r->frames;
    const uint32_t macs    = samples * r->stages * DSP_IIR_MACS_PER_SAMPLE_STAGE;

    /* Cycles available in one 32-frame block at 48 kHz. */
    const uint32_t block_cycles = scaled_div(cpu, r->frames, AUDIO_FS_HZ);

    console_printf("  cycles/block    : min %u, mean %u, max %u\n",
                   r->cycles_min, r->cycles_mean, r->cycles_max);
    print_milli("  cycles/sample   : min ",
                scaled_div(r->cycles_min, 1000u, samples), "");
    print_milli("  cyc/sample/stage: min ",
                scaled_div(r->cycles_min, 1000u, samples * r->stages), "");
    print_milli("  cycles/MAC      : min ",
                scaled_div(r->cycles_min, 1000u, macs), "(DF2T = 5 MAC/sample/stage)");

    print_milli("  time/block      : min ",
                scaled_div(r->cycles_min, 1000000u, cpu / 1000u), "us");
    print_milli("                    mean ",
                scaled_div(r->cycles_mean, 1000000u, cpu / 1000u), "us");

    if (block_cycles != 0u)
    {
        print_milli("  share of block  : min ",
                    scaled_div(r->cycles_min, 100000u, block_cycles), "%");
        print_milli("                    mean ",
                    scaled_div(r->cycles_mean, 100000u, block_cycles), "%");
        console_printf("  (one %u-frame block at %u Hz = %u cycles at %u Hz)\n",
                       r->frames, (uint32_t)AUDIO_FS_HZ, block_cycles, cpu);

        /*
         * N_max is the protocol's headline figure for the challenge: sections
         * per channel that fit one block period, kernel only, at 4 DSP
         * channels. It folds the clock rate into the declared block budget.
         */
        console_printf("  N_max (%u ch)    : %u sections per channel fit one block"
                       " (kernel only)\n",
                       (uint32_t)DSP_TIER2_DSP_CHANNELS,
                       n_max_sections(r, block_cycles));
        /* The feasibility threshold, derived from this build's own clock
         * rather than quoted from the protocol document: a hard-coded 7.44
         * would silently become wrong the day the clock changes. */
        const uint32_t threshold_milli =
            scaled_div(block_cycles, 1000u,
                       DSP_TIER2_DSP_CHANNELS * r->frames
                           * DSP_BENCH_CHALLENGE_STAGES);

        console_printf("  challenge       : %u sections needs <= %u.%03u"
                       " cyc/sample/section\n",
                       (uint32_t)DSP_BENCH_CHALLENGE_STAGES,
                       threshold_milli / 1000u, threshold_milli % 1000u);
    }

    /*
     * The protocol's mandatory re-check rule, printed by the harness rather
     * than left in a document nobody rereads at the bench.
     */
    const uint32_t per_ss_milli =
        scaled_div(r->cycles_min, 1000u, samples * r->stages);

    if (r->valid && (per_ss_milli < (DSP_BENCH_RECHECK_MILLI)))
    {
        console_printf("  *** %u.%03u cyc/sample/section is below the %u.000"
                       " re-check threshold.\n",
                       per_ss_milli / 1000u, per_ss_milli % 1000u,
                       DSP_BENCH_RECHECK_MILLI / 1000u);
        console_writeln("  *** Do NOT record this as an official result until it has"
                        " been re-measured");
        console_writeln("  *** and the hot-loop disassembly re-read.");
    }

    /*
     * The frozen vendor-kernel prediction, checked against this measurement.
     *
     * C = max(issue floor, dependent ops * L). Both bracketing quantities came
     * out of this build's own disassembly, so the only unknown is L, the
     * dependent-result latency of the FPU multiply-accumulate. Printing the L
     * that the measurement implies is what turns the prediction into something
     * that can fail: an implied L outside roughly 1..6 cycles means the
     * instrument is wrong, not that the core is surprising.
     *
     * Below the issue floor there is nothing to interpret. Fewer cycles than
     * instructions is impossible on a single-issue core, so the harness refuses
     * the number outright rather than reporting a record.
     */
#if defined(DSP_USE_M33_DF2T_OPT) && (DSP_USE_M33_DF2T_OPT != 0)
    if (r->valid)
    {
        console_writeln("  arithmetic      : 5 VMUL + 4 VADD per sample/section");
        console_writeln("  prediction      : vendor VFMA latency model is not applicable");
        console_writeln("  verification    : use 'f' probe and linked-image disassembly");
    }
#else
    if (r->valid)
    {
        const uint32_t implied_l_milli =
            scaled_div(per_ss_milli, 1000u, DSP_BENCH_DEP_OPS_MILLI);

        console_printf("  predicted       : max(%u.%03u issue, %u.%03u dep ops"
                       " x L) cyc/sample/section\n",
                       DSP_BENCH_ISSUE_FLOOR_MILLI / 1000u,
                       DSP_BENCH_ISSUE_FLOOR_MILLI % 1000u,
                       DSP_BENCH_DEP_OPS_MILLI / 1000u,
                       DSP_BENCH_DEP_OPS_MILLI % 1000u);
        console_printf("  implied L       : %u.%03u cycles per dependent FPU MAC\n",
                       implied_l_milli / 1000u, implied_l_milli % 1000u);

        if (per_ss_milli < DSP_BENCH_ISSUE_FLOOR_MILLI)
        {
            console_printf("  *** %u.%03u cyc/sample/section is BELOW the %u.%03u"
                           " instruction issue floor.\n",
                           per_ss_milli / 1000u, per_ss_milli % 1000u,
                           DSP_BENCH_ISSUE_FLOOR_MILLI / 1000u,
                           DSP_BENCH_ISSUE_FLOOR_MILLI % 1000u);
            console_writeln("  *** This is not a fast result, it is an impossible"
                            " one: a single-issue core");
            console_writeln("  *** cannot retire more instructions than cycles."
                            " Fix the instrument.");
        }
        else if (implied_l_milli < 1000u)
        {
            console_writeln("  *** Implied L is below 1 cycle. Check the counter"
                            " and the geometry before recording.");
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Sweeps                                                              */
/* ------------------------------------------------------------------ */
/*
 * A sweep is printed as one compact line per point. The reason to sweep at
 * all is that a single operating point cannot separate the per-sample cost
 * from the fixed per-call cost: the difference between adjacent points can,
 * and the fixed cost is what an 8-call-per-block wrapper pays 8 times.
 */
static void sweep_line(const dsp_bench_result_t *r)
{
    if (!r->valid)
    {
        console_printf("  %2u ch  %2u st : (no timing)\n", r->channels, r->stages);
        return;
    }

    const uint32_t cpu     = board_cpu_clock_hz();
    const uint32_t samples = r->channels * r->frames;
    const uint32_t per_ss  = scaled_div(r->cycles_min, 1000u,
                                        samples * r->stages);
    const uint32_t block_cycles = scaled_div(cpu, r->frames, AUDIO_FS_HZ);

    console_printf("  %2u ch  %2u st : min %7u cyc  %u.%03u cyc/sample/stage"
                   "  N_max %3u\n",
                   r->channels, r->stages, r->cycles_min,
                   per_ss / 1000u, per_ss % 1000u,
                   n_max_sections(r, block_cycles));
}

void dsp_bench_sweep_stages(void)
{
    static const uint32_t points[] = DSP_BENCH_SWEEP_STAGE_POINTS;
    dsp_bench_result_t r;

    /*
     * The long sweep runs on the allpass bank throughout, including at the
     * short points. Two reasons, both about not comparing different things:
     * the Butterworth bank is only 6 sections long so it cannot reach the top
     * of the sweep at all, and mixing two filter families inside one sweep
     * would put a coefficient change inside a length comparison.
     */
    console_printf("stage sweep at %u ch x %u frames, bank %s"
                   " (%u warm-up blocks):\n",
                   (uint32_t)DSP_BENCH_DEFAULT_CHANNELS,
                   (uint32_t)DSP_BENCH_DEFAULT_FRAMES,
                   s_bank_ap.name,
                   (uint32_t)DSP_VECTORS_AP_WARMUP_BLOCKS);
    console_printf("  N_max = sections per channel that fit one %u-frame block"
                   " at %u ch, kernel only\n",
                   (uint32_t)DSP_BENCH_DEFAULT_FRAMES,
                   (uint32_t)DSP_TIER2_DSP_CHANNELS);

    for (uint32_t i = 0u; i < (sizeof(points) / sizeof(points[0])); i++)
    {
        if (dsp_bench_run_bank(DSP_BENCH_DEFAULT_CHANNELS,
                               DSP_BENCH_DEFAULT_FRAMES,
                               points[i], DSP_BENCH_ITERATIONS,
                               DSP_BENCH_BANK_AP84, &r))
        {
            sweep_line(&r);
        }
    }
}

void dsp_bench_run_challenge(void)
{
    dsp_bench_result_t r;

    console_printf("\nchallenge point: %u ch x %u frames x %u sections"
                   " (the Sonora Classic/DRC record geometry)\n",
                   (uint32_t)DSP_TIER2_DSP_CHANNELS,
                   (uint32_t)DSP_BENCH_DEFAULT_FRAMES,
                   (uint32_t)DSP_BENCH_CHALLENGE_STAGES);

    if (dsp_bench_run_bank(DSP_TIER2_DSP_CHANNELS,
                           DSP_BENCH_DEFAULT_FRAMES,
                           DSP_BENCH_CHALLENGE_STAGES,
                           DSP_BENCH_ITERATIONS,
                           DSP_BENCH_BANK_AP84, &r))
    {
        dsp_bench_print_result(&r);
    }
    else
    {
        console_writeln("  refused - geometry does not fit");
    }
}

/* ------------------------------------------------------------------ */
/* Tier 2                                                              */
/* ------------------------------------------------------------------ */
static dsp_tier2_t s_tier2;
static int32_t     s_t2_tx[DSP_VECTORS_FRAMES * DSP_TIER2_OUT_SLOTS];

/*
 * Worst absolute deviation of the Tier 2 output from the generated reference,
 * in whole 32-bit slot units. Integer, because both sides are integers - a
 * float comparison here would invent a tolerance where the data has none.
 */
static uint32_t tier2_once(dsp_iir_fault_t iir_fault, dsp_tier2_fault_t fault)
{
    if (!dsp_tier2_configure(&s_tier2, DSP_VECTORS_STAGES,
                             dsp_coeffs_bw12_lp, DSP_COEFFS_BW12_LP_STAGES,
                             iir_fault, fault))
    {
        return 0xFFFFFFFFu;     /* could not configure: reported as a failure */
    }

    memset(s_t2_tx, 0, sizeof(s_t2_tx));

    dsp_tier2_reset(&s_tier2);
    dsp_tier2_process(&s_tier2, dsp_vectors_t2_rx, s_t2_tx, DSP_VECTORS_FRAMES);

    uint32_t worst = 0u;

    for (uint32_t i = 0u;
         i < (DSP_VECTORS_FRAMES * DSP_TIER2_OUT_SLOTS);
         i++)
    {
        const int64_t got = (int64_t)s_t2_tx[i];
        const int64_t ref = (int64_t)dsp_vectors_t2_tx[i];
        const int64_t d   = (got > ref) ? (got - ref) : (ref - got);

        if ((uint64_t)d > (uint64_t)worst)
        {
            worst = (d > 0xFFFFFFFF) ? 0xFFFFFFFFu : (uint32_t)d;
        }
    }

    return worst;
}

bool dsp_bench_self_test_tier2(void)
{
    bool ok = true;

    console_printf("KAT [tier2]: %u slots in -> %u ch -> %u ch DF2T x %u"
                   " -> %u slots out, tolerance %d\n",
                   (uint32_t)DSP_TIER2_IN_SLOTS,
                   (uint32_t)DSP_TIER2_IN_CHANNELS,
                   (uint32_t)DSP_TIER2_DSP_CHANNELS,
                   (uint32_t)DSP_VECTORS_STAGES,
                   (uint32_t)DSP_TIER2_OUT_SLOTS,
                   (int32_t)DSP_VECTORS_T2_TOLERANCE);

    const uint32_t clean = tier2_once(DSP_IIR_FAULT_NONE, DSP_TIER2_FAULT_NONE);
    const bool clean_pass = (clean <= (uint32_t)DSP_VECTORS_T2_TOLERANCE);

    console_printf("  clean run          : max |err| = %u slot units -> %s\n",
                   clean, clean_pass ? "PASS" : "FAIL");
    ok = clean_pass;

    /* Tier-2-specific defects. */
    for (uint32_t f = (uint32_t)DSP_TIER2_FAULT_NONE + 1u;
         f < (uint32_t)DSP_TIER2_FAULT_COUNT;
         f++)
    {
        const dsp_tier2_fault_t fault = (dsp_tier2_fault_t)f;
        const uint32_t w = tier2_once(DSP_IIR_FAULT_NONE, fault);
        const bool caught = (w > (uint32_t)DSP_VECTORS_T2_TOLERANCE);

        console_printf("  fault run          : max |err| = %u -> %s  [%s]\n",
                       w, caught ? "caught, as required" : "NOT CAUGHT - FAIL",
                       dsp_tier2_fault_name(fault));
        if (!caught)
        {
            ok = false;
        }
    }

    /* And the kernel defects, through the Tier 2 chain: the expansion and the
     * conversions must not be able to mask a broken cascade. */
    for (uint32_t f = (uint32_t)DSP_IIR_FAULT_NONE + 1u;
         f < (uint32_t)DSP_IIR_FAULT_COUNT;
         f++)
    {
        const dsp_iir_fault_t fault = (dsp_iir_fault_t)f;
        const uint32_t w = tier2_once(fault, DSP_TIER2_FAULT_NONE);
        const bool caught = (w > (uint32_t)DSP_VECTORS_T2_TOLERANCE);

        console_printf("  fault run          : max |err| = %u -> %s  [kernel: %s]\n",
                       w, caught ? "caught, as required" : "NOT CAUGHT - FAIL",
                       dsp_iir_fault_name(fault));
        if (!caught)
        {
            ok = false;
        }
    }

    console_printf("  verdict [tier2]    : %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

void dsp_bench_run_tier2(uint32_t stages)
{
    if (!s_ready)
    {
        return;
    }

    const dsp_bench_bank_t bank = (stages > DSP_COEFFS_BW12_LP_STAGES)
                                      ? DSP_BENCH_BANK_AP84
                                      : DSP_BENCH_BANK_BW12_LP;
    const kat_bank_t *const b =
        (bank == DSP_BENCH_BANK_AP84) ? &s_bank_ap : &s_bank_lp;

    console_printf("\nTier 2 (DRC-equivalent DSP block): %u slots in -> %u ch"
                   " -> %u ch x %u sections -> %u slots out, bank %s\n",
                   (uint32_t)DSP_TIER2_IN_SLOTS,
                   (uint32_t)DSP_TIER2_IN_CHANNELS,
                   (uint32_t)DSP_TIER2_DSP_CHANNELS,
                   stages,
                   (uint32_t)DSP_TIER2_OUT_SLOTS,
                   b->name);

    if (!dsp_tier2_configure(&s_tier2, stages, b->coeffs, b->coeff_stages,
                             DSP_IIR_FAULT_NONE, DSP_TIER2_FAULT_NONE))
    {
        console_writeln("  refused - cascade longer than the coefficient bank");
        return;
    }

    dsp_tier2_reset(&s_tier2);

    const uint32_t warmup = (b->warmup_blocks > 4u) ? b->warmup_blocks : 4u;

    for (uint32_t i = 0u; i < warmup; i++)
    {
        dsp_tier2_process(&s_tier2, dsp_vectors_t2_rx, s_t2_tx,
                          DSP_VECTORS_FRAMES);
    }

    if (dsp_cycles_source() == DSP_CYCLES_SRC_NONE)
    {
        console_writeln("  timing unavailable - no usable cycle counter");
        return;
    }

    const uint32_t overhead = dsp_cycles_overhead();
    const uint32_t span     = dsp_cycles_span();
    const uint32_t limit    = span / 4u;
    uint32_t min = 0xFFFFFFFFu;
    uint32_t max = 0u;
    uint64_t sum = 0u;
    bool     fits = true;

    for (uint32_t i = 0u; i < DSP_BENCH_ITERATIONS; i++)
    {
        const uint32_t t0 = dsp_cycles_now();
        dsp_tier2_process(&s_tier2, dsp_vectors_t2_rx, s_t2_tx,
                          DSP_VECTORS_FRAMES);
        const uint32_t t1 = dsp_cycles_now();

        uint32_t d = dsp_cycles_delta(t0, t1);

        if (d > limit)
        {
            fits = false;
            break;
        }

        d = (d > overhead) ? (d - overhead) : 0u;

        if (d < min) { min = d; }
        if (d > max) { max = d; }
        sum += d;
    }

    s_sink = (uint32_t)s_t2_tx[0];

    if (!fits)
    {
        console_writeln("  measured interval does not fit the counter - refused");
        return;
    }

    const uint32_t cpu  = board_cpu_clock_hz();
    const uint32_t mean = (uint32_t)(sum / (uint64_t)DSP_BENCH_ITERATIONS);
    const uint32_t block_cycles = scaled_div(cpu, DSP_VECTORS_FRAMES, AUDIO_FS_HZ);

    console_printf("  cycles/block    : min %u, mean %u, max %u\n", min, mean, max);
    print_milli("  time/block      : min ",
                scaled_div(min, 1000000u, cpu / 1000u), "us");

    if (block_cycles != 0u)
    {
        print_milli("  share of block  : min ",
                    scaled_div(min, 100000u, block_cycles), "%");
    }

    /*
     * The Tier 1 run of the same cascade at the same channel count, so the
     * conversion-and-expansion cost is the printed difference rather than
     * something the reader has to derive from two separate sessions. This is
     * the number that says how much of Tier 2 is not the kernel.
     */
    dsp_bench_result_t t1;

    if (dsp_bench_run_bank(DSP_TIER2_DSP_CHANNELS, DSP_VECTORS_FRAMES, stages,
                           DSP_BENCH_ITERATIONS, bank, &t1) && t1.valid)
    {
        console_printf("  kernel only     : min %u cycles (Tier 1, same geometry)\n",
                       t1.cycles_min);

        if (min > t1.cycles_min)
        {
            const uint32_t glue = min - t1.cycles_min;

            console_printf("  conversion+expand: %u cycles = ", glue);
            print_milli("", scaled_div(glue, 100000u, min), "% of Tier 2");
        }
        else
        {
            console_writeln("  conversion+expand: not resolvable - Tier 2 measured"
                            " no slower than the kernel alone");
        }
    }
}

void dsp_bench_sweep_channels(void)
{
    dsp_bench_result_t r;

    console_printf("channel sweep at %u frames x %u stages:\n",
                   (uint32_t)DSP_BENCH_DEFAULT_FRAMES,
                   (uint32_t)DSP_BENCH_DEFAULT_STAGES);

    for (uint32_t ch = 1u; ch <= DSP_BENCH_MAX_CHANNELS; ch++)
    {
        if (dsp_bench_run(ch,
                          DSP_BENCH_DEFAULT_FRAMES,
                          DSP_BENCH_DEFAULT_STAGES,
                          DSP_BENCH_ITERATIONS, &r))
        {
            sweep_line(&r);
        }
    }
}

/* ------------------------------------------------------------------ */
/* DWT auxiliary counter diagnostic                                    */
/* ------------------------------------------------------------------ */
#if APP_ENABLE_DWT_DIAG
/*
 * Diagnostic, never a benchmark metric. The official figure stays the CYCCNT
 * measurement in dsp_bench_run(), whose timed region this function does not
 * touch: this is a separate run, at Tier 1 geometry (1 channel), driving the
 * same kernel through the same dsp_iir_process().
 *
 * Three geometries are run rather than one. The counters' widths are unknown -
 * the TRM defers the register descriptions to the Armv8-M Architecture
 * Reference Manual (page 78), which this project does not hold - so a single
 * large geometry could silently saturate an 8-bit counter and report a
 * plausible undercount. Running 1, 6 and 84 sections makes the width visible:
 * a counter that scales with the first two and then pins is narrow, and the
 * max-per-chunk figures say where it pinned.
 */
static void dwt_diag_one(uint32_t stages, const dsp_dwt_caps_t *caps)
{
    const kat_bank_t *const b =
        (stages > s_bank_lp.coeff_stages) ? &s_bank_ap : &s_bank_lp;

    if (!dsp_iir_configure(&s_filter, 1u, stages,
                           b->coeffs, b->coeff_stages, DSP_IIR_FAULT_NONE))
    {
        console_printf("  %u sections: refused - geometry\n", stages);
        return;
    }

    dsp_signal_fill_lcg(s_in, DSP_BENCH_DEFAULT_FRAMES, 0u);
    dsp_iir_reset(&s_filter);

    const uint32_t warmup = (b->warmup_blocks > 4u) ? b->warmup_blocks : 4u;
    for (uint32_t i = 0u; i < warmup; i++)
    {
        dsp_iir_process(&s_filter, s_in, s_out, DSP_BENCH_DEFAULT_FRAMES);
    }

    dsp_dwt_acc_t acc;
    memset(&acc, 0, sizeof(acc));

    uint32_t cycles = 0u;

    /* One chunk == one block, because dsp_iir_process() is the unit the
     * benchmark times and subdividing it would measure something else. The
     * counters are drained between chunks, outside the timed region. */
    for (uint32_t i = 0u; i < DSP_BENCH_DWT_DIAG_CHUNKS; i++)
    {
        dsp_dwt_diag_zero();

        const uint32_t t0 = dsp_cycles_now();
        dsp_iir_process(&s_filter, s_in, s_out, DSP_BENCH_DEFAULT_FRAMES);
        const uint32_t t1 = dsp_cycles_now();

        dsp_dwt_diag_drain(&acc);

        uint32_t d = dsp_cycles_delta(t0, t1);
        if (d > dsp_cycles_overhead())
        {
            d -= dsp_cycles_overhead();
        }
        cycles += d;
    }

    s_sink += (uint32_t)s_out[0];

    /* Instructions issued, from the disassembly of this very build: the body
     * is DSP_BENCH_ISSUE_FLOOR_MILLI thousandths of an instruction per sample
     * per section. Prologue and epilogue are excluded, which is why the
     * residual is a model check and not an identity. */
    const uint32_t issued =
        scaled_div(DSP_BENCH_ISSUE_FLOOR_MILLI,
                   DSP_BENCH_DEFAULT_FRAMES * stages
                       * DSP_BENCH_DWT_DIAG_CHUNKS,
                   1000u);

    console_printf("--- Tier 1, 1 ch x %u frames x %u sections, bank %s\n",
                   (uint32_t)DSP_BENCH_DEFAULT_FRAMES, stages, b->name);
    dsp_dwt_diag_print_acc(&acc, caps, cycles, issued);
}

void dsp_bench_run_dwt_diag(void)
{
    dsp_dwt_caps_t caps;

    if (!s_ready)
    {
        console_writeln("dwt diag: refused - bench not initialised");
        return;
    }

    if (!dsp_dwt_diag_probe(&caps))
    {
        dsp_dwt_diag_print_caps(&caps);
        console_writeln("dwt diag: refused - CYCCNT does not advance, so no"
                        " cycle accounting is possible");
        return;
    }

    dsp_dwt_diag_print_caps(&caps);

    dwt_diag_one(1u, &caps);
    dwt_diag_one(DSP_BENCH_DEFAULT_STAGES, &caps);
    dwt_diag_one(DSP_BENCH_CHALLENGE_STAGES, &caps);

    console_writeln("dwt diag: diagnostic only. None of the above is a"
                    " benchmark result.");
}
#endif /* APP_ENABLE_DWT_DIAG */

#endif /* APP_ENABLE_DSP_BENCH */
