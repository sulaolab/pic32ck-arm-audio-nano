/*
 * board.c - clocks, cache policy, pin muxing, LED, delays.
 *
 * Register field names come exclusively from PIC32CK-SG DFP 1.6.199
 * component headers. The clock start-up order follows Microchip's own
 * MCC/Harmony clock generator for this family
 * (csp/peripheral/clk_pic32ck_gc_sg/templates/plib_clock.c.ftl):
 *
 *      OSC32KCTRL.CLKSELCTRL
 *   -> [PLL0: SUPC additional regulator, PLL0 dividers, PLL0CTRL, wait lock]
 *   -> MCLK.CLKDIV (wait CKRDY)
 *   -> GCLK.GENCTRL[0] (wait SYNCBUSY)
 *   -> GCLK.PCHCTRL[n] for each peripheral channel
 *
 * The DFLL is never programmed: OSCCTRL.DFLLCTRLA reset value is 0x82
 * (ENABLE | ONDEMAND) and DFLLCTRLB reset value is 0x00 (LOOPEN = 0), so
 * DFLL48M is already running open-loop at 48 MHz out of reset. That is
 * also exactly what MCC emits for its default DFLL settings: with
 * OPMODE = Open and ONDEMAND = Enable its template generates no DFLL
 * code at all.
 */
#include "board.h"

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */

static void clock_osc32kctrl_init(void)
{
    /* Leave the RTC and HSM 32 kHz selections at the internal ultra
     * low-power oscillator. This write only makes the reset state
     * explicit; no 32.768 kHz crystal is assumed anywhere. */
    OSC32KCTRL_REGS->OSC32KCTRL_CLKSELCTRL =
        OSC32KCTRL_CLKSELCTRL_RTCSEL(0u) | OSC32KCTRL_CLKSELCTRL_HSMSEL(0u);
}

#if (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_PLL120)
static void clock_pll0_init(void)
{
    /* The additional core voltage regulator must be up before FDPLL0 is
     * enabled (this is the first thing MCC's PLL0_Initialize() does). */
    SUPC_REGS->SUPC_VREGCTRL |= SUPC_VREGCTRL_AVREGEN_Msk;
    while ((SUPC_REGS->SUPC_STATUS & SUPC_STATUS_ADDVREGRDY_Msk) != SUPC_STATUS_ADDVREGRDY_Msk)
    {
        /* wait for the additional regulator */
    }

    /* fPFD = fREF / REFDIV = 48 MHz / 48 = 1 MHz */
    OSCCTRL_REGS->OSCCTRL_PLL0REFDIV = OSCCTRL_PLL0REFDIV_REFDIV(APP_PLL0_REFDIV);

    /* fVCO = fPFD * FBDIV = 1 MHz * 240 = 240 MHz */
    OSCCTRL_REGS->OSCCTRL_PLL0FBDIV = OSCCTRL_PLL0FBDIV_FBDIV(APP_PLL0_FBDIV);

    /* Fractional divider unused: INTDIV = 0 bypasses it. */
    OSCCTRL_REGS->OSCCTRL_FRACDIV0 =
        OSCCTRL_FRACDIV0_INTDIV(0u) | OSCCTRL_FRACDIV0_REMDIV(0u);
    while ((OSCCTRL_REGS->OSCCTRL_SYNCBUSY & OSCCTRL_SYNCBUSY_FRACDIV0_Msk) == OSCCTRL_SYNCBUSY_FRACDIV0_Msk)
    {
        /* wait for FRACDIV0 synchronization */
    }

    /* Output 0 only: fOUT0 = fVCO / POSTDIV0 = 240 MHz / 2 = 120 MHz */
    OSCCTRL_REGS->OSCCTRL_PLL0POSTDIVA =
        OSCCTRL_PLL0POSTDIVA_OUTEN0_Msk | OSCCTRL_PLL0POSTDIVA_POSTDIV0(APP_PLL0_POSTDIV0);

    /* Clear a stale lock-rise flag first: it is only meaningful the first time
     * it sets after this enable (DS60001795 OSCCTRL INTFLAG.PLL0LOCKR). */
    OSCCTRL_REGS->OSCCTRL_INTFLAG = OSCCTRL_INTFLAG_PLL0LOCKR_Msk;

    OSCCTRL_REGS->OSCCTRL_PLL0CTRL |=
        OSCCTRL_PLL0CTRL_BWSEL(APP_PLL0_BWSEL) |
        OSCCTRL_PLL0CTRL_REFSEL(OSCCTRL_PLL0CTRL_REFSEL_DFLL48M_Val) |
        OSCCTRL_PLL0CTRL_ENABLE_Msk;

    /* The datasheet's lock indicator is INTFLAG.PLL0LOCKR ("the frequency of
     * CLK_PLL is stable when PLLLOCKR in INTFLAG is set"). DFP 1.10.278 no
     * longer defines a STATUS.PLL0LOCK bit, so the flag is the one to poll. */
    while ((OSCCTRL_REGS->OSCCTRL_INTFLAG & OSCCTRL_INTFLAG_PLL0LOCKR_Msk) != OSCCTRL_INTFLAG_PLL0LOCKR_Msk)
    {
        /* wait for PLL0 lock */
    }
}
#endif /* PLL120 */

static void clock_gclk_generator_init(uint32_t generator, uint32_t src, uint32_t div)
{
    GCLK_REGS->GCLK_GENCTRL[generator] =
        GCLK_GENCTRL_SRC(src) | GCLK_GENCTRL_DIV(div) | GCLK_GENCTRL_GENEN_Msk;

    while ((GCLK_REGS->GCLK_SYNCBUSY & (GCLK_SYNCBUSY_GENCTRL0_Msk << generator)) != 0u)
    {
        /* wait for this generator to synchronize */
    }
}

static void clock_init(void)
{
    clock_osc32kctrl_init();

#if (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_PLL120)
    clock_pll0_init();
#endif

    /* CPU clock = MAINCK / CLKDIV. DIV1 keeps them equal.
     * Flash access time needs no attention: FCR.CTRLA reset value is
     * 0x8000, i.e. AUTOWS = 1, so the flash controller derives its own
     * wait states from the AHB clock. */
    MCLK_REGS->MCLK_CLKDIV = MCLK_CLKDIV_DIV(APP_MCLK_CLKDIV);
    while ((MCLK_REGS->MCLK_INTFLAG & MCLK_INTFLAG_CKRDY_Msk) != MCLK_INTFLAG_CKRDY_Msk)
    {
        /* wait for the main clock to be ready */
    }

    /* GCLK0 feeds MAINCK / the CPU. */
#if (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_PLL120)
    clock_gclk_generator_init(APP_GCLK_GEN_CPU, GCLK_GENCTRL_SRC_PLL0_0_Val, 1u);
#else
    clock_gclk_generator_init(APP_GCLK_GEN_CPU, GCLK_GENCTRL_SRC_DFLL_Val, 1u);
#endif

    /* SERCOM core clock: DFLL48M / 1 = 48 MHz, independent of the CPU
     * profile so baud rates do not move with the CPU clock. */
    clock_gclk_generator_init(APP_GCLK_GEN_SERCOM_CORE, GCLK_GENCTRL_SRC_DFLL_Val, 1u);

    /* SERCOM slow clock: OSCULP32K / 1 = 32.768 kHz. Required by the I2C
     * host for SDA hold timing; internal oscillator, no crystal. */
    clock_gclk_generator_init(APP_GCLK_GEN_SERCOM_SLOW, GCLK_GENCTRL_SRC_OSCULP32K_Val, 1u);
}

/* ------------------------------------------------------------------ */
/* Cache policy                                                       */
/* ------------------------------------------------------------------ */

static void cache_init(void)
{
    /*
     * CMCC (Cortex M Cache Controller) is disabled out of reset:
     * CMCC.CTRL reset value = 0x00 (CEN = 0), and the XC32 device
     * start-up file for this part never touches it. So "D-cache off" is
     * already true before this function runs; the code below only makes
     * the policy explicit and, optionally, turns the instruction side on.
     *
     * D-cache stays OFF for first light so that no DMA buffer needs
     * cache maintenance. Revisit for a cache-enabled performance configuration.
     */
    board_periph_clock_enable(BOARD_MCLK_ID_CMCC_AHB);

    /* Cache configuration can only be changed while the controller is
     * disabled. */
    CMCC_REGS->CMCC_CTRL &= ~CMCC_CTRL_CEN_Msk;
    while ((CMCC_REGS->CMCC_SR & CMCC_SR_CSTS_Msk) != 0u)
    {
        /* wait for the cache controller to go inactive */
    }

#if APP_ENABLE_ICACHE
    /* Instruction caching on, data caching off. */
    CMCC_REGS->CMCC_CFG = (CMCC_REGS->CMCC_CFG & ~(CMCC_CFG_ICDIS_Msk | CMCC_CFG_DCDIS_Msk)) |
                          CMCC_CFG_DCDIS_Msk;
    CMCC_REGS->CMCC_CTRL |= CMCC_CTRL_CEN_Msk;
#else
    /* Both sides off: leave the controller disabled. */
    CMCC_REGS->CMCC_CFG |= CMCC_CFG_ICDIS_Msk | CMCC_CFG_DCDIS_Msk;
#endif
}

/* ------------------------------------------------------------------ */
/* Peripheral clock helpers                                           */
/* ------------------------------------------------------------------ */

void board_periph_clock_enable(uint32_t mclk_id)
{
    /* MCLK.CLKMSK is a flat array of 9 x 32 enable bits indexed by the
     * MCLK_ID_AHB / MCLK_ID_APB numbers from the device ATDF. */
    MCLK_REGS->MCLK_CLKMSK[mclk_id / 32u] |= (uint32_t)1u << (mclk_id % 32u);
}

void board_gclk_channel_enable(uint32_t gclk_id, uint32_t generator)
{
    GCLK_REGS->GCLK_PCHCTRL[gclk_id] = GCLK_PCHCTRL_GEN(generator) | GCLK_PCHCTRL_CHEN_Msk;
    while ((GCLK_REGS->GCLK_PCHCTRL[gclk_id] & GCLK_PCHCTRL_CHEN_Msk) != GCLK_PCHCTRL_CHEN_Msk)
    {
        /* wait for the channel to synchronize */
    }
}

/* ------------------------------------------------------------------ */
/* PORT                                                               */
/* ------------------------------------------------------------------ */

/*
 * This image is linked SECURE (TrustZone-M; linker -DSECURE). PORT.NONSEC
 * resets to 0, so every pin is a secure pin, and DS60001795 PORT.NONSEC says a
 * secure pin's configuration "is only available through the secure alias.
 * Attempt to change the pin configuration through the non-secure alias will be
 * silently ignored and reads will return 0." PORT_REGS (0x44800000) is that
 * non-secure alias. Microchip's own PORT plib uses PORT_SEC in a TrustZone
 * project for the same reason (csp port_u2210 config/port.py, PORT_REG_NAME).
 */
#define BOARD_PORT  PORT_SEC_REGS

void board_pin_mux(uint32_t group, uint32_t pin, uint32_t mux, bool input_enable)
{
    uint8_t pmux = BOARD_PORT->GROUP[group].PORT_PMUX[pin / 2u];
    uint8_t cfg  = (uint8_t)PORT_PINCFG_PMUXEN_Msk;

    if ((pin & 1u) != 0u)
    {
        pmux = (uint8_t)((pmux & (uint8_t)~PORT_PMUX_PMUXO_Msk) | (uint8_t)PORT_PMUX_PMUXO(mux));
    }
    else
    {
        pmux = (uint8_t)((pmux & (uint8_t)~PORT_PMUX_PMUXE_Msk) | (uint8_t)PORT_PMUX_PMUXE(mux));
    }
    BOARD_PORT->GROUP[group].PORT_PMUX[pin / 2u] = pmux;

    if (input_enable)
    {
        cfg |= (uint8_t)PORT_PINCFG_INEN_Msk;
    }
    BOARD_PORT->GROUP[group].PORT_PINCFG[pin] = cfg;
}

bool board_pin_read(uint32_t group, uint32_t pin)
{
    return (BOARD_PORT->GROUP[group].PORT_IN & ((uint32_t)1u << pin)) != 0u;
}

void board_pin_output(uint32_t group, uint32_t pin, bool initial_level)
{
    const uint32_t mask = (uint32_t)1u << pin;

    if (initial_level)
    {
        BOARD_PORT->GROUP[group].PORT_OUTSET = mask;
    }
    else
    {
        BOARD_PORT->GROUP[group].PORT_OUTCLR = mask;
    }

    BOARD_PORT->GROUP[group].PORT_PINCFG[pin] = 0u;   /* plain GPIO, no PMUX */
    BOARD_PORT->GROUP[group].PORT_DIRSET = mask;
}

/* ------------------------------------------------------------------ */
/* LED                                                                */
/* ------------------------------------------------------------------ */

void board_led_set(bool on)
{
    const uint32_t mask = (uint32_t)1u << BOARD_LED0_PIN;

#if BOARD_LED0_ACTIVE_LOW
    const bool level = !on;
#else
    const bool level = on;
#endif

    if (level)
    {
        BOARD_PORT->GROUP[BOARD_LED0_PORT].PORT_OUTSET = mask;
    }
    else
    {
        BOARD_PORT->GROUP[BOARD_LED0_PORT].PORT_OUTCLR = mask;
    }
}

void board_led_toggle(void)
{
    BOARD_PORT->GROUP[BOARD_LED0_PORT].PORT_OUTTGL = (uint32_t)1u << BOARD_LED0_PIN;
}

/* ------------------------------------------------------------------ */
/* Delay                                                              */
/* ------------------------------------------------------------------ */

void board_delay_us(uint32_t us)
{
    /* One SysTick reload per microsecond. At 120 MHz that is 120 cycles, so
     * the handful of cycles spent in the polling loop makes the delay a few
     * per cent long - acceptable for register settling, and never short. */
    SysTick->LOAD = (APP_CPU_CLOCK_HZ / 1000000u) - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    while (us-- != 0u)
    {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0u)
        {
            /* wait one microsecond */
        }
    }

    SysTick->CTRL = 0u;
}

void board_delay_ms(uint32_t ms)
{
    /* Polled SysTick, no interrupt. LOAD is 24 bits; at 120 MHz a 1 ms
     * reload is 120000, well inside range. */
    SysTick->LOAD = (APP_CPU_CLOCK_HZ / 1000u) - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    while (ms-- != 0u)
    {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0u)
        {
            /* wait one millisecond */
        }
    }

    SysTick->CTRL = 0u;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                          */
/* ------------------------------------------------------------------ */

uint32_t board_cpu_clock_hz(void)
{
    return (uint32_t)APP_CPU_CLOCK_HZ;
}

uint32_t board_sercom_core_clock_hz(void)
{
    return (uint32_t)APP_GCLK_SERCOM_CORE_HZ;
}

const char *board_clock_profile_name(void)
{
#if (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_PLL120)
    return "DFLL48M -> FDPLL0 -> GCLK0";
#else
    return "DFLL48M -> GCLK0";
#endif
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

void board_init(void)
{
    clock_init();
    cache_init();

    board_periph_clock_enable(BOARD_MCLK_ID_PORT_APB);

    /* LED off first, then drive the pin, so it never blinks on reset. */
#if BOARD_LED0_ACTIVE_LOW
    board_pin_output(BOARD_LED0_PORT, BOARD_LED0_PIN, true);
#else
    board_pin_output(BOARD_LED0_PORT, BOARD_LED0_PIN, false);
#endif
}
