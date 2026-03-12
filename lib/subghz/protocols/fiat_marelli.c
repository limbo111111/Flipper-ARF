#include "fiat_marelli.h"
#include <inttypes.h>
#include <lib/toolbox/manchester_decoder.h>

#define TAG "FiatMarelli"

//   Suspected Magneti Marelli BSI keyfob protocol
//   Found on: Fiat Panda (and possibly other Fiat/Lancia/Alfa ~2003-2012)
//
//   RF: 433.92 MHz, Manchester encoding
//   te_short ~260us, te_long ~520us
//   Preamble: ~191 short-short pairs (alternating 260us HIGH/LOW)
//   Gap: ~3126us LOW
//   Sync: ~2065us HIGH
//   Data: 88 Manchester bits (often decoded as 104 with 16-bit 0xFFFF preamble residue)
//   Retransmissions: 7-10 per press
//
//   Frame layout (after stripping 16-bit 0xFFFF preamble):
//     Bytes 0-3: Fixed ID / Serial (32 bits)
//     Byte 4:    Button (upper nibble) | Type (lower nibble, always 0x2)
//     Bytes 5-10: Rolling/encrypted code (48 bits)
#define FIAT_MARELLI_PREAMBLE_MIN  200  // Min preamble pulses (100 pairs)
#define FIAT_MARELLI_GAP_MIN       2500 // Gap detection threshold (us)
#define FIAT_MARELLI_SYNC_MIN      1500 // Sync pulse minimum (us)
#define FIAT_MARELLI_SYNC_MAX      2600 // Sync pulse maximum (us)
#define FIAT_MARELLI_MAX_DATA_BITS 104  // Max data bits to collect (13 bytes)

static const SubGhzBlockConst subghz_protocol_fiat_marelli_const = {
    .te_short = 260,
    .te_long = 520,
    .te_delta = 80,
    .min_count_bit_for_found = 80,
};

struct SubGhzProtocolDecoderFiatMarelli {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t decoder_state;
    uint16_t preamble_count;
    uint8_t raw_data[13];    // Up to 104 bits (13 bytes)
    uint8_t bit_count;
    uint32_t extra_data;     // Bits beyond first 64, right-aligned
    uint32_t te_last;
};

struct SubGhzProtocolEncoderFiatMarelli {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    FiatMarelliDecoderStepReset = 0,
    FiatMarelliDecoderStepPreamble = 1,
    FiatMarelliDecoderStepSync = 2,
    FiatMarelliDecoderStepData = 3,
} FiatMarelliDecoderStep;

// ============================================================================
// PROTOCOL INTERFACE DEFINITIONS
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_fiat_marelli_decoder = {
    .alloc = subghz_protocol_decoder_fiat_marelli_alloc,
    .free = subghz_protocol_decoder_fiat_marelli_free,
    .feed = subghz_protocol_decoder_fiat_marelli_feed,
    .reset = subghz_protocol_decoder_fiat_marelli_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_marelli_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_marelli_serialize,
    .deserialize = subghz_protocol_decoder_fiat_marelli_deserialize,
    .get_string = subghz_protocol_decoder_fiat_marelli_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_marelli_encoder = {
    .alloc = subghz_protocol_encoder_fiat_marelli_alloc,
    .free = subghz_protocol_encoder_fiat_marelli_free,
    .deserialize = subghz_protocol_encoder_fiat_marelli_deserialize,
    .stop = subghz_protocol_encoder_fiat_marelli_stop,
    .yield = subghz_protocol_encoder_fiat_marelli_yield,
};

const SubGhzProtocol subghz_protocol_fiat_marelli = {
    .name = FIAT_MARELLI_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_fiat_marelli_decoder,
    .encoder = &subghz_protocol_fiat_marelli_encoder,
};

// ============================================================================
// ENCODER STUBS (decode-only protocol)
// ============================================================================

void* subghz_protocol_encoder_fiat_marelli_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatMarelli* instance = calloc(1, sizeof(SubGhzProtocolEncoderFiatMarelli));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_marelli;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_fiat_marelli_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatMarelli* instance = context;
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_marelli_deserialize(void* context, FlipperFormat* flipper_format) {
    UNUSED(context);
    UNUSED(flipper_format);
    return SubGhzProtocolStatusError; 
}

void subghz_protocol_encoder_fiat_marelli_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatMarelli* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_fiat_marelli_yield(void* context) {
    UNUSED(context);
    return level_duration_reset();
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

// Helper: rebuild raw_data[] from generic.data + extra_data
static void fiat_marelli_rebuild_raw_data(SubGhzProtocolDecoderFiatMarelli* instance) {
    memset(instance->raw_data, 0, sizeof(instance->raw_data));

    // First 64 bits from generic.data
    uint64_t key = instance->generic.data;
    for(int i = 0; i < 8; i++) {
        instance->raw_data[i] = (uint8_t)(key >> (56 - i * 8));
    }

    // Remaining bits from extra_data (right-aligned)
    uint8_t extra_bits =
        instance->generic.data_count_bit > 64 ? (instance->generic.data_count_bit - 64) : 0;
    for(uint8_t i = 0; i < extra_bits && i < 32; i++) {
        uint8_t byte_idx = 8 + (i / 8);
        uint8_t bit_pos = 7 - (i % 8);
        if(instance->extra_data & (1UL << (extra_bits - 1 - i))) {
            instance->raw_data[byte_idx] |= (1 << bit_pos);
        }
    }

    instance->bit_count = instance->generic.data_count_bit;
}

void* subghz_protocol_decoder_fiat_marelli_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatMarelli* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderFiatMarelli));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_marelli;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_fiat_marelli_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;
    free(instance);
}

void subghz_protocol_decoder_fiat_marelli_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;
    instance->decoder_state = FiatMarelliDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->extra_data = 0;
    instance->te_last = 0;
    instance->generic.data = 0;
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_fiat_marelli_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;
    uint32_t te_short = (uint32_t)subghz_protocol_fiat_marelli_const.te_short;
    uint32_t te_long = (uint32_t)subghz_protocol_fiat_marelli_const.te_long;
    uint32_t te_delta = (uint32_t)subghz_protocol_fiat_marelli_const.te_delta;
    uint32_t diff;

    switch(instance->decoder_state) {
    case FiatMarelliDecoderStepReset:
        // Wait for first short HIGH pulse to start preamble
        if(!level) return;
        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);
        if(diff < te_delta) {
            instance->decoder_state = FiatMarelliDecoderStepPreamble;
            instance->preamble_count = 1;
            instance->te_last = duration;
        }
        break;

    case FiatMarelliDecoderStepPreamble:
        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);

        if(diff < te_delta) {
            // Short pulse (HIGH or LOW)  preamble continues
            instance->preamble_count++;
            instance->te_last = duration;
        } else if(!level && duration > FIAT_MARELLI_GAP_MIN) {
            // Long LOW  potential gap after preamble
            if(instance->preamble_count >= FIAT_MARELLI_PREAMBLE_MIN) {
                instance->decoder_state = FiatMarelliDecoderStepSync;
                instance->te_last = duration;
            } else {
                instance->decoder_state = FiatMarelliDecoderStepReset;
            }
        } else {
            instance->decoder_state = FiatMarelliDecoderStepReset;
        }
        break;

    case FiatMarelliDecoderStepSync:
        // Expect sync HIGH pulse ~2065us after the gap
        if(level && duration >= FIAT_MARELLI_SYNC_MIN && duration <= FIAT_MARELLI_SYNC_MAX) {
            // Sync detected  prepare for Manchester data
            instance->bit_count = 0;
            instance->extra_data = 0;
            instance->generic.data = 0;
            memset(instance->raw_data, 0, sizeof(instance->raw_data));
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
            instance->decoder_state = FiatMarelliDecoderStepData;
            instance->te_last = duration;
        } else {
            instance->decoder_state = FiatMarelliDecoderStepReset;
        }
        break;

    case FiatMarelliDecoderStepData: {
        ManchesterEvent event = ManchesterEventReset;
        bool frame_complete = false;

        // Classify duration as short or long Manchester edge
        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);
        if(diff < te_delta) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        } else {
            diff = (duration > te_long) ? (duration - te_long) : (te_long - duration);
            if(diff < te_delta) {
                event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            }
        }

        if(event != ManchesterEventReset) {
            bool data_bit;
            if(manchester_advance(
                   instance->manchester_state,
                   event,
                   &instance->manchester_state,
                   &data_bit)) {
                uint32_t new_bit = data_bit ? 1 : 0;

                if(instance->bit_count < FIAT_MARELLI_MAX_DATA_BITS) {
                    uint8_t byte_idx = instance->bit_count / 8;
                    uint8_t bit_pos = 7 - (instance->bit_count % 8);
                    if(new_bit) {
                        instance->raw_data[byte_idx] |= (1 << bit_pos);
                    }
                }

                if(instance->bit_count < 64) {
                    instance->generic.data = (instance->generic.data << 1) | new_bit;
                } else {
                    instance->extra_data = (instance->extra_data << 1) | new_bit;
                }

                instance->bit_count++;

                if(instance->bit_count >= FIAT_MARELLI_MAX_DATA_BITS) {
                    frame_complete = true;
                }
            }
        } else {
            if(instance->bit_count >= subghz_protocol_fiat_marelli_const.min_count_bit_for_found) {
                frame_complete = true;
            } else {
                instance->decoder_state = FiatMarelliDecoderStepReset;
            }
        }

        if(frame_complete) {
            instance->generic.data_count_bit = instance->bit_count;

            // Frame layout: bytes 0-1 are 0xFFFF preamble residue
            // Bytes 2-5: Fixed ID (serial)
            // Byte 6: Button (upper nibble) | subtype (lower nibble)
            // Bytes 7-12: Rolling/encrypted code (48 bits)
            instance->generic.serial =
                ((uint32_t)instance->raw_data[2] << 24) |
                ((uint32_t)instance->raw_data[3] << 16) |
                ((uint32_t)instance->raw_data[4] << 8) |
                ((uint32_t)instance->raw_data[5]);
            instance->generic.btn = (instance->raw_data[6] >> 4) & 0xF;
            instance->generic.cnt =
                ((uint32_t)instance->raw_data[7] << 16) |
                ((uint32_t)instance->raw_data[8] << 8) |
                ((uint32_t)instance->raw_data[9]);

            FURI_LOG_I(
                TAG,
                "Decoded %d bits: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                instance->bit_count,
                instance->raw_data[0],
                instance->raw_data[1],
                instance->raw_data[2],
                instance->raw_data[3],
                instance->raw_data[4],
                instance->raw_data[5],
                instance->raw_data[6],
                instance->raw_data[7],
                instance->raw_data[8],
                instance->raw_data[9],
                instance->raw_data[10]);

            if(instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder_state = FiatMarelliDecoderStepReset;
        }

        instance->te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_fiat_marelli_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit =
            instance->generic.data_count_bit > 64 ? 64 : instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_marelli_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        // Save extra data (bits 64+ right-aligned in uint32_t)
        flipper_format_write_uint32(flipper_format, "Extra", &instance->extra_data, 1);

        // Save total bit count explicitly (generic serialize also saves it, but Extra needs context)
        uint32_t extra_bits = instance->generic.data_count_bit > 64
                                  ? (instance->generic.data_count_bit - 64)
                                  : 0;
        flipper_format_write_uint32(flipper_format, "Extra_bits", &extra_bits, 1);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_marelli_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t extra = 0;
        if(flipper_format_read_uint32(flipper_format, "Extra", &extra, 1)) {
            instance->extra_data = extra;
        }

        fiat_marelli_rebuild_raw_data(instance);
    }

    return ret;
}

static const char* fiat_marelli_button_name(uint8_t btn) {
    switch(btn) {
    case 0x2:
        return "Btn A";
    case 0x4:
        return "Btn B";
    default:
        return "Unknown";
    }
}

void subghz_protocol_decoder_fiat_marelli_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMarelli* instance = context;

    uint8_t total_bytes = (instance->bit_count + 7) / 8;
    if(total_bytes > 13) total_bytes = 13;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%08lX Btn:%s(0x%X)\r\n"
        "Roll:%02X%02X%02X%02X%02X%02X\r\n"
        "Data:",
        instance->generic.protocol_name,
        instance->bit_count,
        instance->generic.serial,
        fiat_marelli_button_name(instance->generic.btn),
        instance->generic.btn,
        instance->raw_data[7],
        instance->raw_data[8],
        instance->raw_data[9],
        (total_bytes > 10) ? instance->raw_data[10] : 0,
        (total_bytes > 11) ? instance->raw_data[11] : 0,
        (total_bytes > 12) ? instance->raw_data[12] : 0);

    for(uint8_t i = 0; i < total_bytes; i++) {
        furi_string_cat_printf(output, "%02X", instance->raw_data[i]);
    }
    furi_string_cat_printf(output, "\r\n");
}
