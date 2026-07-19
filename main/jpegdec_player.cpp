#include "jpegdec_player.h"

#include "JPEGDEC.h"

static JPEGDEC s_decoder;
static jpegdec_player_draw_callback_t s_draw_callback;
static void *s_draw_context;

static int jpegdec_draw_adapter(JPEGDRAW *draw)
{
    if (s_draw_callback == nullptr || draw == nullptr) {
        return 0;
    }
    return s_draw_callback(draw->x, draw->y, draw->iWidthUsed, draw->iHeight,
                           draw->pPixels, s_draw_context);
}

extern "C" esp_err_t jpegdec_player_decode(uint8_t *data, size_t size,
                                             jpegdec_player_draw_callback_t draw_cb,
                                             void *user_ctx, int output_scale,
                                             int *width, int *height)
{
    if (data == nullptr || size == 0 || size > INT32_MAX || draw_cb == nullptr ||
        (output_scale != 1 && output_scale != 2)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_draw_callback = draw_cb;
    s_draw_context = user_ctx;
    if (!s_decoder.openRAM(data, static_cast<int>(size), jpegdec_draw_adapter)) {
        s_draw_callback = nullptr;
        s_draw_context = nullptr;
        return ESP_FAIL;
    }

    /* A full 480-pixel MCU row minimizes QSPI transactions (17 per frame). */
    s_decoder.setPixelType(RGB565_BIG_ENDIAN);
    s_decoder.setMaxOutputSize(30);
    s_decoder.setUserPointer(user_ctx);
    if (width != nullptr) {
        *width = (s_decoder.getWidth() + output_scale - 1) / output_scale;
    }
    if (height != nullptr) {
        *height = (s_decoder.getHeight() + output_scale - 1) / output_scale;
    }

    const int options = output_scale == 2 ? JPEG_SCALE_HALF : 0;
    const bool decoded = s_decoder.decode(0, 0, options) != 0;
    s_decoder.close();
    s_draw_callback = nullptr;
    s_draw_context = nullptr;
    return decoded ? ESP_OK : ESP_FAIL;
}
