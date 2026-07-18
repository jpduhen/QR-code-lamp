#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return true from this callback to stop MP3 playback at the next decoded frame. */
typedef bool (*mp3_stop_callback_t)(void *context);

/** Decode an MP3 from the FAT filesystem and play it through the NS4168 I2S output. */
esp_err_t mp3_play_file(const char *path, const volatile int *volume_percent,
                        mp3_stop_callback_t stop_callback, void *stop_context);

#ifdef __cplusplus
}
#endif
