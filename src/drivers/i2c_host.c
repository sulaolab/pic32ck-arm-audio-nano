/*
 * i2c_host.c - SERCOM0 I2C host, 400 kHz, polled, always bounded.
 *
 * The transaction shape (smart mode, repeated START by re-writing ADDR,
 * ACKACT + CMD(3) before the final read byte, wait for BUSSTATE == IDLE
 * after STOP) mirrors Microchip's own SERCOM u2201 I2C master PLIB
 * (csp/peripheral/sercom_u2201/templates/plib_sercom_i2c_master.c.ftl),
 * which is where the two field values without DFP captions come from:
 *   CTRLB.CMD(3)      = issue STOP
 *   STATUS.BUSSTATE(1)= bus IDLE
 */
#include "i2c_host.h"

#include "board.h"

#define I2C_SERCOM_REGS     SERCOM0_REGS

/*
 * SCL generator, from the formula Microchip's MCC uses for this SERCOM:
 *
 *      fSCL = fGCLK / (10 + 2*BAUD + fGCLK * Trise)
 *
 * solved for BAUD and rounded UP so that fSCL never exceeds the target:
 *
 *      BAUD = ceil( (fGCLK/fSCL - 10 - fGCLK*Trise) / 2 )
 *
 * fGCLK = 48 MHz, fSCL = 400 kHz, Trise = 100 ns gives BAUD = 53 and an
 * actual fSCL of 48e6/(10 + 106 + 4.8) = 397.4 kHz.
 *
 * Everything below is scaled by 1000 to stay in integer arithmetic;
 * BAUDLOW stays 0, which makes SCL low and high symmetric.
 */
#define I2C_NCYC_X1000      (((uint64_t)APP_GCLK_SERCOM_CORE_HZ * 1000ull) / (uint64_t)APP_I2C_SCL_HZ)
#define I2C_NRISE_X1000     (((uint64_t)APP_GCLK_SERCOM_CORE_HZ * (uint64_t)APP_I2C_TRISE_NS) / 1000000ull)
#define I2C_BAUD_REG        ((uint32_t)((I2C_NCYC_X1000 - 10000ull - I2C_NRISE_X1000 + 1999ull) / 2000ull))

/* Roughly 10 ms of spinning, expressed in CPU clocks so that it tracks
 * the clock profile. Only used as a runaway guard - a healthy transfer
 * completes in microseconds. */
#define I2C_TIMEOUT_SPINS   (APP_CPU_CLOCK_HZ / 100u)

#define I2C_BUSSTATE_IDLE   1u
#define I2C_CMD_STOP        3u

static void i2c_sync(void)
{
    while ((I2C_SERCOM_REGS->I2CM.SERCOM_SYNCBUSY & SERCOM_I2CM_SYNCBUSY_SYSOP_Msk) != 0u)
    {
        /* wait for the pending system operation */
    }
}

void i2c_host_init(void)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;

    board_periph_clock_enable(BOARD_MCLK_ID_SERCOM0_APB);
    board_gclk_channel_enable(BOARD_GCLK_ID_SERCOM0_CORE, APP_GCLK_GEN_SERCOM_CORE);
    board_gclk_channel_enable(BOARD_GCLK_ID_SERCOM_SLOW, APP_GCLK_GEN_SERCOM_SLOW);

    /* Both pads must have INEN set: I2C is open-drain and the peripheral
     * reads back SDA and SCL. */
    board_pin_mux(BOARD_I2C_SDA_PORT, BOARD_I2C_SDA_PIN, BOARD_I2C_SDA_MUX, true);
    board_pin_mux(BOARD_I2C_SCL_PORT, BOARD_I2C_SCL_PIN, BOARD_I2C_SCL_MUX, true);

    m->SERCOM_CTRLA = SERCOM_I2CM_CTRLA_SWRST_Msk;
    while ((m->SERCOM_SYNCBUSY & SERCOM_I2CM_SYNCBUSY_SWRST_Msk) != 0u)
    {
        /* wait for software reset */
    }

    m->SERCOM_CTRLA =
        SERCOM_I2CM_CTRLA_MODE_I2C_MASTER |
        SERCOM_I2CM_CTRLA_SPEED_SM |                  /* Sm/Fm, up to 400 kHz */
        SERCOM_I2CM_CTRLA_SDAHOLD_75NS;               /* MCC default          */

    /* Smart mode: reading DATA automatically acknowledges per ACKACT and
     * clocks in the next byte. */
    m->SERCOM_CTRLB = SERCOM_I2CM_CTRLB_SMEN_Msk;

    m->SERCOM_BAUD = SERCOM_I2CM_BAUD_BAUD(I2C_BAUD_REG) | SERCOM_I2CM_BAUD_BAUDLOW(0u);

    m->SERCOM_CTRLA |= SERCOM_I2CM_CTRLA_ENABLE_Msk;
    while ((m->SERCOM_SYNCBUSY & SERCOM_I2CM_SYNCBUSY_ENABLE_Msk) != 0u)
    {
        /* wait for enable */
    }

    /* Force the bus state machine to IDLE; out of reset it reports
     * UNKNOWN and would refuse to start a transfer. */
    m->SERCOM_STATUS = (uint16_t)SERCOM_I2CM_STATUS_BUSSTATE(I2C_BUSSTATE_IDLE);
    i2c_sync();
}

static i2c_status_t i2c_wait_idle(void)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;
    uint32_t spins = I2C_TIMEOUT_SPINS;

    while (spins-- != 0u)
    {
        if ((m->SERCOM_STATUS & SERCOM_I2CM_STATUS_BUSSTATE_Msk) ==
            SERCOM_I2CM_STATUS_BUSSTATE(I2C_BUSSTATE_IDLE))
        {
            return I2C_OK;
        }
    }
    return I2C_ERR_BUS_STUCK;
}

/* Wait for one of the two "operation done" flags, or an error. */
static i2c_status_t i2c_wait_flag(uint8_t flag_msk)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;
    uint32_t spins = I2C_TIMEOUT_SPINS;

    while (spins-- != 0u)
    {
        const uint8_t f = m->SERCOM_INTFLAG;

        if ((f & SERCOM_I2CM_INTFLAG_ERROR_Msk) != 0u)
        {
            m->SERCOM_INTFLAG = (uint8_t)SERCOM_I2CM_INTFLAG_ERROR_Msk;
            return I2C_ERR_BUS;
        }
        if ((f & flag_msk) != 0u)
        {
            return I2C_OK;
        }
    }
    return I2C_ERR_TIMEOUT;
}

static void i2c_stop(void)
{
    I2C_SERCOM_REGS->I2CM.SERCOM_CTRLB |= SERCOM_I2CM_CTRLB_CMD(I2C_CMD_STOP);
    i2c_sync();
    (void)i2c_wait_idle();
}

static bool i2c_saw_nack(void)
{
    return (I2C_SERCOM_REGS->I2CM.SERCOM_STATUS & SERCOM_I2CM_STATUS_RXNACK_Msk) != 0u;
}

/* Address phase. `read` selects the R/W bit. */
static i2c_status_t i2c_start(uint8_t addr7, bool read)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;
    const uint32_t addr = ((uint32_t)addr7 << 1u) | (read ? 1u : 0u);

    /* Clear sticky status before the transfer so RXNACK reflects this one. */
    m->SERCOM_STATUS = (uint16_t)(SERCOM_I2CM_STATUS_BUSERR_Msk |
                                  SERCOM_I2CM_STATUS_ARBLOST_Msk |
                                  SERCOM_I2CM_STATUS_RXNACK_Msk |
                                  SERCOM_I2CM_STATUS_LOWTOUT_Msk);
    i2c_sync();

    m->SERCOM_ADDR = SERCOM_I2CM_ADDR_ADDR(addr);
    i2c_sync();

    /* A write address completes with MB, a read address with SB (the
     * first data byte has already been clocked in by smart mode). */
    const i2c_status_t st = i2c_wait_flag(read
                                              ? (uint8_t)SERCOM_I2CM_INTFLAG_SB_Msk
                                              : (uint8_t)SERCOM_I2CM_INTFLAG_MB_Msk);
    if (st != I2C_OK)
    {
        return st;
    }
    if (i2c_saw_nack())
    {
        return I2C_ERR_NACK;
    }
    return I2C_OK;
}

static i2c_status_t i2c_send_bytes(const uint8_t *data, uint32_t len)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;

    for (uint32_t i = 0u; i < len; i++)
    {
        m->SERCOM_DATA = (uint32_t)data[i];
        i2c_sync();

        const i2c_status_t st = i2c_wait_flag((uint8_t)SERCOM_I2CM_INTFLAG_MB_Msk);
        if (st != I2C_OK)
        {
            return st;
        }
        if (i2c_saw_nack())
        {
            return I2C_ERR_NACK;
        }
    }
    return I2C_OK;
}

i2c_status_t i2c_host_write(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    i2c_status_t st = i2c_wait_idle();
    if (st != I2C_OK)
    {
        return st;
    }

    st = i2c_start(addr7, false);
    if (st == I2C_OK)
    {
        st = i2c_send_bytes(data, len);
    }

    i2c_stop();
    return st;
}

i2c_status_t i2c_host_write_read(uint8_t addr7,
                                 const uint8_t *wdata, uint32_t wlen,
                                 uint8_t *rdata, uint32_t rlen)
{
    sercom_i2cm_registers_t *const m = &I2C_SERCOM_REGS->I2CM;

    i2c_status_t st = i2c_wait_idle();
    if (st != I2C_OK)
    {
        return st;
    }

    if (wlen != 0u)
    {
        st = i2c_start(addr7, false);
        if (st == I2C_OK)
        {
            st = i2c_send_bytes(wdata, wlen);
        }
        if (st != I2C_OK)
        {
            i2c_stop();
            return st;
        }
    }

    if (rlen == 0u)
    {
        i2c_stop();
        return I2C_OK;
    }

    /* Repeated START: writing ADDR again while we own the bus. Smart mode
     * clocks in the first byte, so SB is already set when this returns. */
    st = i2c_start(addr7, true);
    if (st != I2C_OK)
    {
        i2c_stop();
        return st;
    }

    for (uint32_t i = 0u; i < rlen; i++)
    {
        if (i == (rlen - 1u))
        {
            /* Last byte: arm NACK and STOP *before* the read, because in
             * smart mode the DATA read is what releases the bus. */
            m->SERCOM_CTRLB |= SERCOM_I2CM_CTRLB_ACKACT_Msk |
                               SERCOM_I2CM_CTRLB_CMD(I2C_CMD_STOP);
            i2c_sync();
        }
        else
        {
            m->SERCOM_CTRLB &= ~SERCOM_I2CM_CTRLB_ACKACT_Msk;
            i2c_sync();
        }

        rdata[i] = (uint8_t)m->SERCOM_DATA;

        if (i != (rlen - 1u))
        {
            st = i2c_wait_flag((uint8_t)SERCOM_I2CM_INTFLAG_SB_Msk);
            if (st != I2C_OK)
            {
                i2c_stop();
                return st;
            }
        }
    }

    /* Leave ACKACT clear for the next transfer. */
    m->SERCOM_CTRLB &= ~SERCOM_I2CM_CTRLB_ACKACT_Msk;
    i2c_sync();

    (void)i2c_wait_idle();
    return I2C_OK;
}

const char *i2c_status_str(i2c_status_t s)
{
    switch (s)
    {
    case I2C_OK:            return "OK";
    case I2C_ERR_TIMEOUT:   return "timeout";
    case I2C_ERR_NACK:      return "NACK (no device at that address?)";
    case I2C_ERR_BUS:       return "bus error";
    case I2C_ERR_BUS_STUCK: return "bus never went idle (SDA/SCL held low?)";
    default:                return "unknown";
    }
}
