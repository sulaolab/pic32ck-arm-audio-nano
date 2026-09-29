/*
 * board_pins.h - EV33V87A / AC164162 pin map.
 *
 * Every pad / PORT group / peripheral-function letter below was read out
 * of the PIC32CK2051SG01064 ATDF shipped with PIC32CK-SG DFP 1.6.199
 * (<instance>/<signals> for each SERCOM, and <pinouts> for the 64-pin
 * package). Peripheral-function letters map to PMUX values through the
 * ATDF value group PORT_PMUX__PMUXE: A=0, B=1, C=2, D=3, ...
 *
 * PORT groups: A=0, B=1, C=2, D=3.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_PORT_A            0u
#define BOARD_PORT_B            1u
#define BOARD_PORT_C            2u
#define BOARD_PORT_D            3u

/* PMUX peripheral function values (ATDF PORT_PMUX__PMUXE). */
#define BOARD_PMUX_A            0x0u
#define BOARD_PMUX_B            0x1u
#define BOARD_PMUX_C            0x2u
#define BOARD_PMUX_D            0x3u

/* ---------------- User LED --------------------------------------------
 * EV33V87A user LED = PD05 (pin 56 of the 64-pin package).
 *
 * Note: PD05 is also SERCOM0/PAD2 function C (ioset 3). This firmware
 * only muxes SERCOM0 PAD0/PAD1 (PD06/PD07) for I2C, so PD05 stays a
 * plain GPIO and there is no conflict.
 *
 * ACTIVE HIGH. EV33V87A User Guide DS50004178A lists "PD05  User LED,
 * active high" and states that "Driving ... to VDDIO will activate the
 * LED". This is NOT the usual Curiosity Nano active-low arrangement, so
 * do not "correct" it back.
 */
#define BOARD_LED0_PORT         BOARD_PORT_D
#define BOARD_LED0_PIN          5u
#ifndef BOARD_LED0_ACTIVE_LOW
#define BOARD_LED0_ACTIVE_LOW   0
#endif

/* ---------------- Console: SERCOM5 USART -----------------------------
 * PA11 = SERCOM5/PAD0 function C (ioset 1) = target TX
 * PA12 = SERCOM5/PAD1 function C (ioset 1) = target RX
 * These are the pins the on-board debugger's CDC gateway is wired to.
 */
#define BOARD_UART_TX_PORT      BOARD_PORT_A
#define BOARD_UART_TX_PIN       11u
#define BOARD_UART_TX_MUX       BOARD_PMUX_C
#define BOARD_UART_RX_PORT      BOARD_PORT_A
#define BOARD_UART_RX_PIN       12u
#define BOARD_UART_RX_MUX       BOARD_PMUX_C

/* SERCOM5 pad usage -> USART pin-out fields.
 * TXPO = PAD0 (DFP: SERCOM_USART_INT_CTRLA_TXPO_PAD0_Val = 0)
 * RXPO = PAD1 (DFP: SERCOM_USART_INT_CTRLA_RXPO_PAD1_Val = 1) */
#define BOARD_UART_TXPO         0u
#define BOARD_UART_RXPO         1u

/* ---------------- WM8904 control: SERCOM0 I2C ------------------------
 * PD06 = SERCOM0/PAD0 function C (ioset 3) = SDA
 * PD07 = SERCOM0/PAD1 function C (ioset 3) = SCL
 */
#define BOARD_I2C_SDA_PORT      BOARD_PORT_D
#define BOARD_I2C_SDA_PIN       6u
#define BOARD_I2C_SDA_MUX       BOARD_PMUX_C
#define BOARD_I2C_SCL_PORT      BOARD_PORT_D
#define BOARD_I2C_SCL_PIN       7u
#define BOARD_I2C_SCL_MUX       BOARD_PMUX_C

/* ---------------- Audio: SERCOM4 framed SPI, mikroBUS slot 1 ---------
 * All four pads use peripheral function C, ioset 1:
 *   PB09 = SERCOM4/PAD0 = DO    (MCU audio data out -> WM8904 DACDAT)
 *   PB10 = SERCOM4/PAD1 = SCK   (BCLK in, WM8904 is clock master)
 *   PC00 = SERCOM4/PAD2 = SS    (FSYNC/LRCLK in)
 *   PC01 = SERCOM4/PAD3 = DI    (MCU audio data in <- WM8904 ADCDAT)
 *
 * The ATDF also lists an ioset-4 mapping on function D which swaps
 * PAD0/PAD1 (PB10 = PAD0, PB09 = PAD1). That alternative is NOT used;
 * this design is fixed on function C / ioset 1.
 */
#define BOARD_AUDIO_DO_PORT     BOARD_PORT_B
#define BOARD_AUDIO_DO_PIN      9u
#define BOARD_AUDIO_DO_MUX      BOARD_PMUX_C
#define BOARD_AUDIO_SCK_PORT    BOARD_PORT_B
#define BOARD_AUDIO_SCK_PIN     10u
#define BOARD_AUDIO_SCK_MUX     BOARD_PMUX_C
#define BOARD_AUDIO_FS_PORT     BOARD_PORT_C
#define BOARD_AUDIO_FS_PIN      0u
#define BOARD_AUDIO_FS_MUX      BOARD_PMUX_C
#define BOARD_AUDIO_DI_PORT     BOARD_PORT_C
#define BOARD_AUDIO_DI_PIN      1u
#define BOARD_AUDIO_DI_MUX      BOARD_PMUX_C

/* SERCOM4 pad usage -> SPI pin-out fields.
 * DOPO = 0: DFP enum SERCOM_SPIS_CTRLA_DOPO_PAD0 (value 0) - data out on
 *           PAD0, which places SCK on PAD1 and SS on PAD2.
 * DIPO = 3: DFP enum SERCOM_SPIS_CTRLA_DIPO_PAD3 (value 3) - data in on
 *           PAD3. */
#define BOARD_AUDIO_DOPO        0u
#define BOARD_AUDIO_DIPO        3u

/* ---------------- Peripheral clock / DMA trigger indices --------------
 * Aliases onto the DFP's own instance macros (the per-instance headers
 * pulled in by the device header) so that no index is transcribed by
 * hand. MCLK.CLKMSK[9] is one flat 288-bit array indexed by these IDs:
 * AHB and APB share the space, and a full dump of every MCLK_ID_AHB /
 * MCLK_ID_APB on this device gives values 3..113 with no duplicates.
 */
#define BOARD_MCLK_ID_DMA0_AHB      DMA0_MCLK_ID_AHB        /* 8  */
#define BOARD_MCLK_ID_CMCC_AHB      CMCC_MCLK_ID_AHB        /* 11 */
#define BOARD_MCLK_ID_PORT_APB      PORT_MCLK_ID_APB        /* 64 */
#define BOARD_MCLK_ID_DMA0_APB      DMA0_MCLK_ID_APB        /* 65 */
#define BOARD_MCLK_ID_SERCOM0_APB   SERCOM0_MCLK_ID_APB     /* 71 */
#define BOARD_MCLK_ID_SERCOM4_APB   SERCOM4_MCLK_ID_APB     /* 96 */
#define BOARD_MCLK_ID_SERCOM5_APB   SERCOM5_MCLK_ID_APB     /* 97 */

/* GCLK peripheral channel indices. GCLK_ID_SLOW is 18 for every SERCOM
 * on this device, i.e. one shared channel. */
#define BOARD_GCLK_ID_SERCOM_SLOW   SERCOM0_GCLK_ID_SLOW    /* 18 */
#define BOARD_GCLK_ID_SERCOM0_CORE  SERCOM0_GCLK_ID_CORE    /* 19 */
#define BOARD_GCLK_ID_SERCOM4_CORE  SERCOM4_GCLK_ID_CORE    /* 25 */
#define BOARD_GCLK_ID_SERCOM5_CORE  SERCOM5_GCLK_ID_CORE    /* 26 */

#endif /* BOARD_PINS_H */
