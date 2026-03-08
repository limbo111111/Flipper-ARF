#include "fiat_mystery.h"
#include <inttypes.h>
#include <lib/toolbox/manchester_decoder.h>

#define TAG "FiatMystery"

//   Fiat Panda "pandarella e tonino":
//   te_short ~260us, te_long ~520us (Manchester)
//   Preamble: ~191 short-short pairs (alternating 260us HIGH/LOW)
//   Gap: ~3126us LOW
//   Sync: ~2065us HIGH
//   Data: 86 Manchester bits
//   Retransmissions: 7-8 per press
#define FIAT_MYSTERY_PREAMBLE_MIN  200  // Min preamble pulses (100 pairs)
#define FIAT_MYSTERY_GAP_MIN       2500 // Gap detection threshold (us)
#define FIAT_MYSTERY_SYNC_MIN      1500 // Sync pulse minimum (us)
#define FIAT_MYSTERY_SYNC_MAX      2600 // Sync pulse maximum (us)
#define FIAT_MYSTERY_MAX_DATA_BITS 96   // Max data bits to collect

static const SubGhzBlockConst subghz_protocol_fiat_mystery_const = {
    .te_short = 260,
    .te_long = 520,
    .te_delta = 80,
    .min_count_bit_for_found = 80,
};

struct SubGhzProtocolDecoderFiatMystery {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t decoder_state;
    uint16_t preamble_count;
    uint8_t raw_data[12];    // Up to 96 bits (12 bytes)
    uint8_t bit_count;
    uint32_t extra_data;     // Bits beyond first 64, right-aligned
    uint32_t te_last;
};

struct SubGhzProtocolEncoderFiatMystery {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    FiatMysteryDecoderStepReset = 0,
    FiatMysteryDecoderStepPreamble = 1,
    FiatMysteryDecoderStepSync = 2,
    FiatMysteryDecoderStepData = 3,
} FiatMysteryDecoderStep;

// ============================================================================
// PROTOCOL INTERFACE DEFINITIONS
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_fiat_mystery_decoder = {
    .alloc = subghz_protocol_decoder_fiat_mystery_alloc,
    .free = subghz_protocol_decoder_fiat_mystery_free,
    .feed = subghz_protocol_decoder_fiat_mystery_feed,
    .reset = subghz_protocol_decoder_fiat_mystery_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_mystery_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_mystery_serialize,
    .deserialize = subghz_protocol_decoder_fiat_mystery_deserialize,
    .get_string = subghz_protocol_decoder_fiat_mystery_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_mystery_encoder = {
    .alloc = subghz_protocol_encoder_fiat_mystery_alloc,
    .free = subghz_protocol_encoder_fiat_mystery_free,
    .deserialize = subghz_protocol_encoder_fiat_mystery_deserialize,
    .stop = subghz_protocol_encoder_fiat_mystery_stop,
    .yield = subghz_protocol_encoder_fiat_mystery_yield,
};

const SubGhzProtocol subghz_protocol_fiat_mystery = {
    .name = FIAT_MYSTERY_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_fiat_mystery_decoder,
    .encoder = &subghz_protocol_fiat_mystery_encoder,
};

// ============================================================================
// ENCODER STUBS (decode-only protocol)
// ============================================================================

void* subghz_protocol_encoder_fiat_mystery_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatMystery* instance = calloc(1, sizeof(SubGhzProtocolEncoderFiatMystery));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_mystery;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_fiat_mystery_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatMystery* instance = context;
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_mystery_deserialize(void* context, FlipperFormat* flipper_format) {
    UNUSED(context);
    UNUSED(flipper_format);
    return SubGhzProtocolStatusError; 
}

void subghz_protocol_encoder_fiat_mystery_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderFiatMystery* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_fiat_mystery_yield(void* context) {
    UNUSED(context);
    return level_duration_reset();
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

// Helper: rebuild raw_data[] from generic.data + extra_data
static void fiat_mystery_rebuild_raw_data(SubGhzProtocolDecoderFiatMystery* instance) {
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

void* subghz_protocol_decoder_fiat_mystery_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatMystery* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderFiatMystery));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_fiat_mystery;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_fiat_mystery_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;
    free(instance);
}

void subghz_protocol_decoder_fiat_mystery_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;
    instance->decoder_state = FiatMysteryDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->extra_data = 0;
    instance->te_last = 0;
    instance->generic.data = 0;
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_fiat_mystery_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;
    uint32_t te_short = (uint32_t)subghz_protocol_fiat_mystery_const.te_short;
    uint32_t te_long = (uint32_t)subghz_protocol_fiat_mystery_const.te_long;
    uint32_t te_delta = (uint32_t)subghz_protocol_fiat_mystery_const.te_delta;
    uint32_t diff;

    switch(instance->decoder_state) {
    case FiatMysteryDecoderStepReset:
        // Wait for first short HIGH pulse to start preamble
        if(!level) return;
        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);
        if(diff < te_delta) {
            instance->decoder_state = FiatMysteryDecoderStepPreamble;
            instance->preamble_count = 1;
            instance->te_last = duration;
        }
        break;

    case FiatMysteryDecoderStepPreamble:
        diff = (duration > te_short) ? (duration - te_short) : (te_short - duration);

        if(diff < te_delta) {
            // Short pulse (HIGH or LOW)  preamble continues
            instance->preamble_count++;
            instance->te_last = duration;
        } else if(!level && duration > FIAT_MYSTERY_GAP_MIN) {
            // Long LOW  potential gap after preamble
            if(instance->preamble_count >= FIAT_MYSTERY_PREAMBLE_MIN) {
                instance->decoder_state = FiatMysteryDecoderStepSync;
                instance->te_last = duration;
            } else {
                instance->decoder_state = FiatMysteryDecoderStepReset;
            }
        } else {
            instance->decoder_state = FiatMysteryDecoderStepReset;
        }
        break;

    case FiatMysteryDecoderStepSync:
        // Expect sync HIGH pulse ~2065us after the gap
        if(level && duration >= FIAT_MYSTERY_SYNC_MIN && duration <= FIAT_MYSTERY_SYNC_MAX) {
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
            instance->decoder_state = FiatMysteryDecoderStepData;
            instance->te_last = duration;
        } else {
            instance->decoder_state = FiatMysteryDecoderStepReset;
        }
        break;

    case FiatMysteryDecoderStepData: {
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

                if(instance->bit_count < FIAT_MYSTERY_MAX_DATA_BITS) {
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

                if(instance->bit_count >= FIAT_MYSTERY_MAX_DATA_BITS) {
                    frame_complete = true;
                }
            }
        } else {
            if(instance->bit_count >= subghz_protocol_fiat_mystery_const.min_count_bit_for_found) {
                frame_complete = true;
            } else {
                instance->decoder_state = FiatMysteryDecoderStepReset;
            }
        }

        if(frame_complete) {
            instance->generic.data_count_bit = instance->bit_count;

            instance->generic.serial =
                ((uint32_t)instance->raw_data[4] << 24) |
                ((uint32_t)instance->raw_data[5] << 16) |
                ((uint32_t)instance->raw_data[6] << 8) |
                ((uint32_t)instance->raw_data[7]);
            instance->generic.cnt = (uint32_t)(instance->generic.data >> 32);
            instance->generic.btn = 0;

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

            instance->decoder_state = FiatMysteryDecoderStepReset;
        }

        instance->te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_fiat_mystery_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit =
            instance->generic.data_count_bit > 64 ? 64 : instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_mystery_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;

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

SubGhzProtocolStatus subghz_protocol_decoder_fiat_mystery_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t extra = 0;
        if(flipper_format_read_uint32(flipper_format, "Extra", &extra, 1)) {
            instance->extra_data = extra;
        }

        fiat_mystery_rebuild_raw_data(instance);
    }

    return ret;
}

void subghz_protocol_decoder_fiat_mystery_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderFiatMystery* instance = context;

    uint8_t total_bytes = (instance->bit_count + 7) / 8;
    if(total_bytes > 12) total_bytes = 12;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Data:",
        instance->generic.protocol_name,
        instance->bit_count,
        (uint32_t)(instance->generic.data >> 32),
        (uint32_t)(instance->generic.data & 0xFFFFFFFF));

    for(uint8_t i = 0; i < total_bytes; i++) {
        furi_string_cat_printf(output, "%02X", instance->raw_data[i]);
    }
    furi_string_cat_printf(output, "\r\n");
}
