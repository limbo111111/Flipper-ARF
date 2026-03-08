#include "rolljam_receiver.h"
#include <furi_hal_subghz.h>
#include <furi_hal_rtc.h>

#define CC_IOCFG0   0x02
#define CC_FIFOTHR  0x03
#define CC_MDMCFG4  0x10
#define CC_MDMCFG3  0x11
#define CC_MDMCFG2  0x12
#define CC_MDMCFG1  0x13
#define CC_MDMCFG0  0x14
#define CC_DEVIATN  0x15
#define CC_MCSM0    0x18
#define CC_FOCCFG   0x19
#define CC_AGCCTRL2 0x1B
#define CC_AGCCTRL1 0x1C
#define CC_AGCCTRL0 0x1D
#define CC_FREND0   0x22
#define CC_FSCAL3   0x23
#define CC_FSCAL2   0x24
#define CC_FSCAL1   0x25
#define CC_FSCAL0   0x26

// ============================================================
// Presets
// ============================================================

static const uint8_t preset_ook_rx[] = {
    CC_IOCFG0,   0x0D,
    CC_FIFOTHR,  0x47,
    CC_MDMCFG4,  0xE7,  // RX BW ~58kHz
    CC_MDMCFG3,  0x32,
    CC_MDMCFG2,  0x30,
    CC_MDMCFG1,  0x00,
    CC_MDMCFG0,  0x00,
    CC_DEVIATN,  0x47,
    CC_MCSM0,    0x18,
    CC_FOCCFG,   0x16,
    CC_AGCCTRL2, 0x07,
    CC_AGCCTRL1, 0x00,
    CC_AGCCTRL0, 0x91,
    CC_FREND0,   0x11,
    CC_FSCAL3,   0xEA,
    CC_FSCAL2,   0x2A,
    CC_FSCAL1,   0x00,
    CC_FSCAL0,   0x1F,
    0x00, 0x00
};

static const uint8_t preset_fsk_rx[] = {
    CC_IOCFG0,   0x0D,
    CC_FIFOTHR,  0x47,
    CC_MDMCFG4,  0xE7,
    CC_MDMCFG3,  0x32,
    CC_MDMCFG2,  0x00,
    CC_MDMCFG1,  0x00,
    CC_MDMCFG0,  0x00,
    CC_DEVIATN,  0x15,
    CC_MCSM0,    0x18,
    CC_FOCCFG,   0x16,
    CC_AGCCTRL2, 0x07,
    CC_AGCCTRL1, 0x00,
    CC_AGCCTRL0, 0x91,
    CC_FREND0,   0x10,
    CC_FSCAL3,   0xEA,
    CC_FSCAL2,   0x2A,
    CC_FSCAL1,   0x00,
    CC_FSCAL0,   0x1F,
    0x00, 0x00
};

static const uint8_t preset_ook_tx[] = {
    CC_IOCFG0,   0x0D,
    CC_FIFOTHR,  0x47,
    CC_MDMCFG4,  0x8C,
    CC_MDMCFG3,  0x32,
    CC_MDMCFG2,  0x30,
    CC_MDMCFG1,  0x00,
    CC_MDMCFG0,  0x00,
    CC_DEVIATN,  0x47,
    CC_MCSM0,    0x18,
    CC_FOCCFG,   0x16,
    CC_AGCCTRL2, 0x07,
    CC_AGCCTRL1, 0x00,
    CC_AGCCTRL0, 0x91,
    CC_FREND0,   0x11,
    CC_FSCAL3,   0xEA,
    CC_FSCAL2,   0x2A,
    CC_FSCAL1,   0x00,
    CC_FSCAL0,   0x1F,
    0x00, 0x00
};

// ============================================================
// Capture state machine
// ============================================================

#define MIN_PULSE_US       50
#define MAX_PULSE_US       5000
#define SILENCE_GAP_US     10000
#define MIN_FRAME_PULSES   40
#define AUTO_ACCEPT_PULSES 150

typedef enum {
    CapWaiting,
    CapRecording,
    CapDone,
} CapState;

static volatile CapState cap_state;
static volatile int cap_valid_count;
static volatile int cap_total_count;
static volatile bool cap_target_first;
static volatile uint32_t cap_callback_count;

static void capture_rx_callback(bool level, uint32_t duration, void* context) {
    RollJamApp* app = context;

    if(!app->raw_capture_active) return;
    if(cap_state == CapDone) return;

    cap_callback_count++;

    RawSignal* target;
    if(cap_target_first) {
        target = &app->signal_first;
        if(target->valid) return;
    } else {
        target = &app->signal_second;
        if(target->valid) return;
    }

    uint32_t dur = duration;
    if(dur > 32767) dur = 32767;

    switch(cap_state) {
    case CapWaiting:
        if(dur >= MIN_PULSE_US && dur <= MAX_PULSE_US) {
            target->size = 0;
            cap_valid_count = 0;
            cap_total_count = 0;
            cap_state = CapRecording;

            int16_t s = level ? (int16_t)dur : -(int16_t)dur;
            target->data[target->size++] = s;
            cap_valid_count++;
            cap_total_count++;
        }
        break;

    case CapRecording:
        if(target->size >= RAW_SIGNAL_MAX_SIZE) {
            if(cap_valid_count >= MIN_FRAME_PULSES) {
                cap_state = CapDone;
            } else {
                target->size = 0;
                cap_valid_count = 0;
                cap_total_count = 0;
                cap_state = CapWaiting;
            }
            return;
        }

        if(dur > SILENCE_GAP_US) {
            if(cap_valid_count >= MIN_FRAME_PULSES) {
                if(target->size < RAW_SIGNAL_MAX_SIZE) {
                    int16_t s = level ? (int16_t)32767 : -32767;
                    target->data[target->size++] = s;
                }
                cap_state = CapDone;
            } else {
                target->size = 0;
                cap_valid_count = 0;
                cap_total_count = 0;
                cap_state = CapWaiting;
            }
            return;
        }

        {
            int16_t s = level ? (int16_t)dur : -(int16_t)dur;
            target->data[target->size++] = s;
            cap_total_count++;

            if(dur >= MIN_PULSE_US && dur <= MAX_PULSE_US) {
                cap_valid_count++;
                if(cap_valid_count >= AUTO_ACCEPT_PULSES) {
                    cap_state = CapDone;
                }
            }
        }
        break;

    case CapDone:
        break;
    }
}

// ============================================================
// Capture start/stop
// ============================================================

void rolljam_capture_start(RollJamApp* app) {
    FURI_LOG_I(TAG, "Capture start: freq=%lu mod=%d", app->frequency, app->mod_index);

    // Full radio reset sequence
    furi_hal_subghz_reset();
    furi_delay_ms(10);
    furi_hal_subghz_idle();
    furi_delay_ms(10);

    const uint8_t* preset;
    switch(app->mod_index) {
    case ModIndex_FM238:
    case ModIndex_FM476:
        preset = preset_fsk_rx;
        break;
    default:
        preset = preset_ook_rx;
        break;
    }

    furi_hal_subghz_load_custom_preset(preset);
    furi_delay_ms(5);

    uint32_t real_freq = furi_hal_subghz_set_frequency(app->frequency);
    FURI_LOG_I(TAG, "Capture: freq set to %lu", real_freq);

    furi_delay_ms(5);

    // Reset state machine
    cap_state = CapWaiting;
    cap_valid_count = 0;
    cap_total_count = 0;
    cap_callback_count = 0;

    // Determine target
    if(!app->signal_first.valid) {
        cap_target_first = true;
        app->signal_first.size = 0;
        app->signal_first.valid = false;
        FURI_LOG_I(TAG, "Capture target: FIRST signal");
    } else {
        cap_target_first = false;
        app->signal_second.size = 0;
        app->signal_second.valid = false;
        FURI_LOG_I(TAG, "Capture target: SECOND signal (first already valid, size=%d)",
                   app->signal_first.size);
    }

    app->raw_capture_active = true;
    furi_hal_subghz_start_async_rx(capture_rx_callback, app);

    FURI_LOG_I(TAG, "Capture: RX STARTED, active=%d, target_first=%d",
               app->raw_capture_active, cap_target_first);
}

void rolljam_capture_stop(RollJamApp* app) {
    if(!app->raw_capture_active) {
        FURI_LOG_W(TAG, "Capture stop: was not active");
        return;
    }

    app->raw_capture_active = false;

    furi_hal_subghz_stop_async_rx();
    furi_delay_ms(5);
    furi_hal_subghz_idle();
    furi_delay_ms(5);

    FURI_LOG_I(TAG, "Capture stopped. callbacks=%lu capState=%d validCnt=%d totalCnt=%d",
               cap_callback_count, cap_state, cap_valid_count, cap_total_count);
    FURI_LOG_I(TAG, "  Sig1: size=%d valid=%d", app->signal_first.size, app->signal_first.valid);
    FURI_LOG_I(TAG, "  Sig2: size=%d valid=%d", app->signal_second.size, app->signal_second.valid);
}

// ============================================================
// Validation
// ============================================================

bool rolljam_signal_is_valid(RawSignal* signal) {
    if(cap_state != CapDone) {
        // Log every few checks so we can see if callbacks are happening
        static int check_count = 0;
        check_count++;
        if(check_count % 10 == 0) {
            FURI_LOG_D(TAG, "Validate: not done yet, state=%d callbacks=%lu valid=%d total=%d sig_size=%d",
                       cap_state, cap_callback_count, cap_valid_count, cap_total_count, signal->size);
        }
        return false;
    }

    if(signal->size < MIN_FRAME_PULSES) return false;

    int good = 0;
    int total = (int)signal->size;

    for(int i = 0; i < total; i++) {
        int16_t val = signal->data[i];
        int16_t abs_val = val > 0 ? val : -val;
        if(abs_val >= MIN_PULSE_US && abs_val <= MAX_PULSE_US) {
            good++;
        }
    }

    int ratio_pct = (total > 0) ? ((good * 100) / total) : 0;

    if(ratio_pct > 50 && good >= MIN_FRAME_PULSES) {
        FURI_LOG_I(TAG, "Signal VALID: %d/%d (%d%%) samples=%d",
                   good, total, ratio_pct, total);
        return true;
    }

    FURI_LOG_D(TAG, "Signal rejected: %d/%d (%d%%), reset", good, total, ratio_pct);
    signal->size = 0;
    cap_state = CapWaiting;
    cap_valid_count = 0;
    cap_total_count = 0;
    return false;
}

// ============================================================
// TX
// ============================================================

typedef struct {
    const int16_t* data;
    size_t size;
    volatile size_t index;
} TxCtx;

static TxCtx g_tx;

static LevelDuration tx_feed(void* context) {
    UNUSED(context);
    if(g_tx.index >= g_tx.size) return level_duration_reset();

    int16_t sample = g_tx.data[g_tx.index++];
    bool level = (sample > 0);
    uint32_t dur = (uint32_t)(sample > 0 ? sample : -sample);

    return level_duration_make(level, dur);
}

void rolljam_transmit_signal(RollJamApp* app, RawSignal* signal) {
    if(!signal->valid || signal->size == 0) {
        FURI_LOG_E(TAG, "TX: no valid signal");
        return;
    }

    FURI_LOG_I(TAG, "TX: %d samples at %lu Hz", signal->size, app->frequency);

    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_delay_ms(10);

    furi_hal_subghz_load_custom_preset(preset_ook_tx);
    uint32_t real_freq = furi_hal_subghz_set_frequency(app->frequency);
    FURI_LOG_I(TAG, "TX: freq=%lu", real_freq);

    g_tx.data = signal->data;
    g_tx.size = signal->size;
    g_tx.index = 0;

    if(!furi_hal_subghz_start_async_tx(tx_feed, NULL)) {
        FURI_LOG_E(TAG, "TX: start failed!");
        furi_hal_subghz_idle();
        return;
    }

    uint32_t timeout = 0;
    while(!furi_hal_subghz_is_async_tx_complete()) {
        furi_delay_ms(5);
        if(++timeout > 2000) {
            FURI_LOG_E(TAG, "TX: timeout!");
            break;
        }
    }

    furi_hal_subghz_stop_async_tx();
    furi_hal_subghz_idle();

    FURI_LOG_I(TAG, "TX: done (%d/%d)", g_tx.index, signal->size);
}

// ============================================================
// Save
// ============================================================

void rolljam_save_signal(RollJamApp* app, RawSignal* signal) {
    if(!signal->valid || signal->size == 0) {
        FURI_LOG_E(TAG, "Save: no signal");
        return;
    }

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    FuriString* path = furi_string_alloc_printf(
        "/ext/subghz/RJ_%04d%02d%02d_%02d%02d%02d.sub",
        dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);

    FURI_LOG_I(TAG, "Saving: %s", furi_string_get_cstr(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/subghz");
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* line = furi_string_alloc();

        furi_string_set(line, "Filetype: Flipper SubGhz RAW File\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(line, "Version: 1\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(line, "Frequency: %lu\n", app->frequency);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        const char* pname;
        switch(app->mod_index) {
        case ModIndex_AM270: pname = "FuriHalSubGhzPresetOok270Async"; break;
        case ModIndex_FM238: pname = "FuriHalSubGhzPreset2FSKDev238Async"; break;
        case ModIndex_FM476: pname = "FuriHalSubGhzPreset2FSKDev476Async"; break;
        default: pname = "FuriHalSubGhzPresetOok650Async"; break;
        }

        furi_string_printf(line, "Preset: %s\n", pname);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(line, "Protocol: RAW\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        size_t i = 0;
        while(i < signal->size) {
            furi_string_set(line, "RAW_Data:");
            size_t end = i + 512;
            if(end > signal->size) end = signal->size;
            for(; i < end; i++) {
                furi_string_cat_printf(line, " %d", signal->data[i]);
            }
            furi_string_cat(line, "\n");
            storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));
        }

        furi_string_free(line);
        FURI_LOG_I(TAG, "Saved: %d samples", signal->size);
    } else {
        FURI_LOG_E(TAG, "Save failed!");
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(path);
}
