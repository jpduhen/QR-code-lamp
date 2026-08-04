/*
 * Small Cinepak-to-RGB565 decoder for the Verhalenlamp AVI experiment.
 *
 * This follows the public Cinepak bitstream structure and the decode flow
 * used by FFmpeg's LGPL Cinepak decoder, but writes directly into the lamp's
 * RGB565 framebuffer and omits palette/Sega container edge cases we do not
 * generate with the local converter.
 */

#include "cinepak_decoder.h"

#include <string.h>

static uint16_t be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

static uint32_t be24(const uint8_t *data)
{
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) |
           (uint32_t)data[2];
}

static uint32_t be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint8_t clip_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8U) << 8) |
           ((uint16_t)(g & 0xFCU) << 3) |
           ((uint16_t)b >> 3);
}

void cinepak_decoder_init(cinepak_decoder_t *decoder, uint16_t width,
                          uint16_t height)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->width = width;
    decoder->height = height;
    decoder->sega_film_skip_bytes = -1;
}

static void decode_codebook(uint16_t codebook[256][4], int chunk_id,
                            const uint8_t *data, size_t size)
{
    const uint8_t *end = data + size;
    uint32_t flag = 0;
    uint32_t mask = 0;
    const int bytes_per_entry = (chunk_id & 0x04) ? 4 : 6;

    for (int i = 0; i < 256; ++i) {
        if ((chunk_id & 0x01) && (mask >>= 1) == 0) {
            if (data + 4 > end) {
                return;
            }
            flag = be32(data);
            data += 4;
            mask = 0x80000000U;
        }
        if ((chunk_id & 0x01) && (flag & mask) == 0) {
            continue;
        }
        if (data + bytes_per_entry > end) {
            return;
        }

        uint8_t y[4];
        for (int k = 0; k < 4; ++k) {
            y[k] = *data++;
        }
        if (bytes_per_entry == 4) {
            for (int k = 0; k < 4; ++k) {
                codebook[i][k] = rgb565(y[k], y[k], y[k]);
            }
        } else {
            const int u = (int8_t)*data++;
            const int v = (int8_t)*data++;
            for (int k = 0; k < 4; ++k) {
                const int r = y[k] + v * 2;
                const int g = y[k] - (u / 2) - v;
                const int b = y[k] + u * 2;
                codebook[i][k] = rgb565(clip_u8(r), clip_u8(g), clip_u8(b));
            }
        }
    }
}

static void put_pixel(cinepak_decoder_t *decoder, uint16_t *frame,
                      int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < decoder->width && y < decoder->height) {
        frame[y * decoder->width + x] = color;
    }
}

static void put_2x2(cinepak_decoder_t *decoder, uint16_t *frame,
                    int x, int y, const uint16_t colors[4])
{
    put_pixel(decoder, frame, x, y, colors[0]);
    put_pixel(decoder, frame, x + 1, y, colors[1]);
    put_pixel(decoder, frame, x, y + 1, colors[2]);
    put_pixel(decoder, frame, x + 1, y + 1, colors[3]);
}

static void put_v1_block(cinepak_decoder_t *decoder, uint16_t *frame,
                         int x, int y, const uint16_t colors[4])
{
    put_pixel(decoder, frame, x, y, colors[0]);
    put_pixel(decoder, frame, x + 1, y, colors[0]);
    put_pixel(decoder, frame, x, y + 1, colors[0]);
    put_pixel(decoder, frame, x + 1, y + 1, colors[0]);
    put_pixel(decoder, frame, x + 2, y, colors[1]);
    put_pixel(decoder, frame, x + 3, y, colors[1]);
    put_pixel(decoder, frame, x + 2, y + 1, colors[1]);
    put_pixel(decoder, frame, x + 3, y + 1, colors[1]);
    put_pixel(decoder, frame, x, y + 2, colors[2]);
    put_pixel(decoder, frame, x + 1, y + 2, colors[2]);
    put_pixel(decoder, frame, x, y + 3, colors[2]);
    put_pixel(decoder, frame, x + 1, y + 3, colors[2]);
    put_pixel(decoder, frame, x + 2, y + 2, colors[3]);
    put_pixel(decoder, frame, x + 3, y + 2, colors[3]);
    put_pixel(decoder, frame, x + 2, y + 3, colors[3]);
    put_pixel(decoder, frame, x + 3, y + 3, colors[3]);
}

static void put_v4_block(cinepak_decoder_t *decoder, uint16_t *frame,
                         int x, int y, const uint16_t cb0[4],
                         const uint16_t cb1[4], const uint16_t cb2[4],
                         const uint16_t cb3[4])
{
    put_2x2(decoder, frame, x, y, cb0);
    put_2x2(decoder, frame, x + 2, y, cb1);
    put_2x2(decoder, frame, x, y + 2, cb2);
    put_2x2(decoder, frame, x + 2, y + 2, cb3);
}

static bool decode_vectors(cinepak_decoder_t *decoder, cinepak_strip_t *strip,
                           int chunk_id, const uint8_t *data, size_t size,
                           uint16_t *frame)
{
    const uint8_t *end = data + size;
    uint32_t flag = 0;
    uint32_t mask = 0;

    for (int y = strip->y1; y < strip->y2; y += 4) {
        for (int x = strip->x1; x < strip->x2; x += 4) {
            if ((chunk_id & 0x01) && (mask >>= 1) == 0) {
                if (data + 4 > end) {
                    return false;
                }
                flag = be32(data);
                data += 4;
                mask = 0x80000000U;
            }
            if (!(chunk_id & 0x01) || (flag & mask)) {
                if (!(chunk_id & 0x02) && (mask >>= 1) == 0) {
                    if (data + 4 > end) {
                        return false;
                    }
                    flag = be32(data);
                    data += 4;
                    mask = 0x80000000U;
                }
                if ((chunk_id & 0x02) || ((~flag) & mask)) {
                    if (data >= end) {
                        return false;
                    }
                    put_v1_block(decoder, frame, x, y,
                                 strip->v1_codebook[*data++]);
                } else if (flag & mask) {
                    if (data + 4 > end) {
                        return false;
                    }
                    const uint16_t *cb0 = strip->v4_codebook[*data++];
                    const uint16_t *cb1 = strip->v4_codebook[*data++];
                    const uint16_t *cb2 = strip->v4_codebook[*data++];
                    const uint16_t *cb3 = strip->v4_codebook[*data++];
                    put_v4_block(decoder, frame, x, y, cb0, cb1, cb2, cb3);
                }
            }
        }
    }
    return true;
}

static bool decode_strip(cinepak_decoder_t *decoder, cinepak_strip_t *strip,
                         const uint8_t *data, size_t size, uint16_t *frame)
{
    if (strip->x2 > decoder->width || strip->y2 > decoder->height ||
        strip->x1 >= strip->x2 || strip->y1 >= strip->y2) {
        return false;
    }
    const uint8_t *end = data + size;
    while (data + 4 <= end) {
        const int chunk_id = data[0];
        const uint32_t chunk_total = be24(&data[1]);
        if (chunk_total < 4) {
            return false;
        }
        data += 4;
        size_t chunk_size = chunk_total - 4;
        if (data + chunk_size > end) {
            chunk_size = end - data;
        }
        switch (chunk_id) {
        case 0x20:
        case 0x21:
        case 0x24:
        case 0x25:
            decode_codebook(strip->v4_codebook, chunk_id, data, chunk_size);
            break;
        case 0x22:
        case 0x23:
        case 0x26:
        case 0x27:
            decode_codebook(strip->v1_codebook, chunk_id, data, chunk_size);
            break;
        case 0x30:
        case 0x31:
        case 0x32:
            return decode_vectors(decoder, strip, chunk_id, data, chunk_size,
                                  frame);
        default:
            break;
        }
        data += chunk_size;
    }
    return false;
}

static bool predecode_check(cinepak_decoder_t *decoder, const uint8_t *data,
                            size_t size)
{
    if (size < 10) {
        return false;
    }
    const uint16_t num_strips = be16(&data[8]);
    const uint32_t encoded_size = be24(&data[1]);
    if (encoded_size == 0 || size < 10 + num_strips * 12U) {
        return false;
    }
    if (decoder->sega_film_skip_bytes < 0) {
        decoder->sega_film_skip_bytes = encoded_size == size ? 0 : 2;
    }
    return size >= 10U + (size_t)decoder->sega_film_skip_bytes +
           (size_t)num_strips * 12U;
}

bool cinepak_decoder_decode(cinepak_decoder_t *decoder, const uint8_t *data,
                            size_t size, uint16_t *rgb565_frame)
{
    if (!predecode_check(decoder, data, size)) {
        return false;
    }
    const int frame_flags = data[0];
    uint16_t num_strips = be16(&data[8]);
    if (num_strips > CINEPAK_MAX_STRIPS) {
        num_strips = CINEPAK_MAX_STRIPS;
    }
    const uint8_t *cursor = data + 10 + decoder->sega_film_skip_bytes;
    const uint8_t *end = data + size;
    uint16_t previous_y2 = 0;

    for (uint16_t index = 0; index < num_strips; ++index) {
        if (cursor + 12 > end) {
            return false;
        }
        cinepak_strip_t *strip = &decoder->strips[index];
        const uint32_t strip_total = be24(&cursor[1]);
        if (strip_total < 12) {
            return false;
        }
        const uint16_t y1 = be16(&cursor[4]);
        strip->x1 = be16(&cursor[6]);
        if (y1 == 0) {
            strip->y1 = previous_y2;
            strip->y2 = previous_y2 + be16(&cursor[8]);
        } else {
            strip->y1 = y1;
            strip->y2 = be16(&cursor[8]);
        }
        strip->x2 = be16(&cursor[10]);
        cursor += 12;
        size_t strip_size = strip_total - 12;
        if (cursor + strip_size > end) {
            strip_size = end - cursor;
        }
        if (index > 0 && !(frame_flags & 0x01)) {
            memcpy(strip->v4_codebook, decoder->strips[index - 1].v4_codebook,
                   sizeof(strip->v4_codebook));
            memcpy(strip->v1_codebook, decoder->strips[index - 1].v1_codebook,
                   sizeof(strip->v1_codebook));
        }
        if (!decode_strip(decoder, strip, cursor, strip_size, rgb565_frame)) {
            return false;
        }
        previous_y2 = strip->y2;
        cursor += strip_size;
    }
    return true;
}

