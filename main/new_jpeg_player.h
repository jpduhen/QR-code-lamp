#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct new_jpeg_player new_jpeg_player_t;

esp_err_t new_jpeg_player_open(new_jpeg_player_t **player);
void new_jpeg_player_close(new_jpeg_player_t *player);
esp_err_t new_jpeg_player_decode(new_jpeg_player_t *player, uint8_t *data,
                                 size_t size, uint16_t *output,
                                 size_t output_pixels, int *width,
                                 int *height);

