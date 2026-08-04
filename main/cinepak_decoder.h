#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CINEPAK_MAX_STRIPS 32

typedef struct {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    uint16_t v1_codebook[256][4];
    uint16_t v4_codebook[256][4];
} cinepak_strip_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    int sega_film_skip_bytes;
    cinepak_strip_t strips[CINEPAK_MAX_STRIPS];
} cinepak_decoder_t;

void cinepak_decoder_init(cinepak_decoder_t *decoder, uint16_t width,
                          uint16_t height);
bool cinepak_decoder_decode(cinepak_decoder_t *decoder, const uint8_t *data,
                            size_t size, uint16_t *rgb565);

