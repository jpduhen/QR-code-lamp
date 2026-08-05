#include "new_jpeg_player.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_jpeg_dec.h"

struct new_jpeg_player {
    jpeg_dec_handle_t decoder;
    jpeg_dec_io_t io;
    jpeg_dec_header_info_t info;
};

static esp_err_t jpeg_error_to_esp(jpeg_error_t error)
{
    switch (error) {
    case JPEG_ERR_OK:
        return ESP_OK;
    case JPEG_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case JPEG_ERR_INVALID_PARAM:
        return ESP_ERR_INVALID_ARG;
    case JPEG_ERR_BAD_DATA:
    case JPEG_ERR_UNSUPPORT_FMT:
    case JPEG_ERR_UNSUPPORT_STD:
        return ESP_ERR_INVALID_RESPONSE;
    case JPEG_ERR_NO_MORE_DATA:
        return ESP_ERR_INVALID_SIZE;
    case JPEG_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

esp_err_t new_jpeg_player_open(new_jpeg_player_t **player)
{
    *player = NULL;
    new_jpeg_player_t *instance = calloc(1, sizeof(*instance));
    if (instance == NULL) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;
    config.block_enable = false;

    const jpeg_error_t result = jpeg_dec_open(&config, &instance->decoder);
    if (result != JPEG_ERR_OK) {
        free(instance);
        return jpeg_error_to_esp(result);
    }
    *player = instance;
    return ESP_OK;
}

void new_jpeg_player_close(new_jpeg_player_t *player)
{
    if (player == NULL) {
        return;
    }
    if (player->decoder != NULL) {
        jpeg_dec_close(player->decoder);
    }
    free(player);
}

esp_err_t new_jpeg_player_decode(new_jpeg_player_t *player, uint8_t *data,
                                 size_t size, uint16_t *output,
                                 size_t output_pixels, int *width,
                                 int *height)
{
    if (player == NULL || data == NULL || output == NULL ||
        size > INT_MAX || output_pixels > INT_MAX / (int)sizeof(uint16_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&player->io, 0, sizeof(player->io));
    memset(&player->info, 0, sizeof(player->info));
    player->io.inbuf = data;
    player->io.inbuf_len = (int)size;

    jpeg_error_t result = jpeg_dec_parse_header(player->decoder, &player->io,
                                                &player->info);
    if (result != JPEG_ERR_OK) {
        return jpeg_error_to_esp(result);
    }
    const size_t required_pixels = (size_t)player->info.width * player->info.height;
    if (required_pixels > output_pixels) {
        return ESP_ERR_NO_MEM;
    }

    player->io.outbuf = (uint8_t *)output;
    result = jpeg_dec_process(player->decoder, &player->io);
    if (result != JPEG_ERR_OK) {
        return jpeg_error_to_esp(result);
    }

    if (width != NULL) {
        *width = player->info.width;
    }
    if (height != NULL) {
        *height = player->info.height;
    }
    return ESP_OK;
}
