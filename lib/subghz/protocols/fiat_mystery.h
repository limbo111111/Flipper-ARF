#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/toolbox/manchester_decoder.h>
#include <flipper_format/flipper_format.h>

#define FIAT_MYSTERY_PROTOCOL_NAME "Fiat Mystery"

typedef struct SubGhzProtocolDecoderFiatMystery SubGhzProtocolDecoderFiatMystery;
typedef struct SubGhzProtocolEncoderFiatMystery SubGhzProtocolEncoderFiatMystery;

extern const SubGhzProtocol subghz_protocol_fiat_mystery;

void* subghz_protocol_decoder_fiat_mystery_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_fiat_mystery_free(void* context);
void subghz_protocol_decoder_fiat_mystery_reset(void* context);
void subghz_protocol_decoder_fiat_mystery_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_fiat_mystery_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_fiat_mystery_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_mystery_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_fiat_mystery_get_string(void* context, FuriString* output);

// Encoder stubs 
void* subghz_protocol_encoder_fiat_mystery_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_fiat_mystery_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_mystery_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_fiat_mystery_stop(void* context);
LevelDuration subghz_protocol_encoder_fiat_mystery_yield(void* context);
