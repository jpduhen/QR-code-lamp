#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int (*jpegdec_player_draw_callback_t)(int x, int y, int width, int height,
                                               const uint16_t *pixels, void *user_ctx);

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes one JPEG in small blocks, without a full-frame framebuffer. */
esp_err_t jpegdec_player_decode(uint8_t *data, size_t size,
                                jpegdec_player_draw_callback_t draw_cb, void *user_ctx,
                                int output_scale, int *width, int *height);

#ifdef __cplusplus
}
#endif
