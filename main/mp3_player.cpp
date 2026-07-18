#include "mp3_player.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "micro_mp3/mp3_decoder.h"

namespace {

constexpr const char *TAG = "mp3_player";
constexpr gpio_num_t AUDIO_BCLK = GPIO_NUM_42;
constexpr gpio_num_t AUDIO_LRCLK = GPIO_NUM_2;
constexpr gpio_num_t AUDIO_DOUT = GPIO_NUM_41;
constexpr size_t INPUT_BYTES = 4096;

static int16_t scale_sample(int16_t sample, int volume)
{
    return static_cast<int16_t>((static_cast<int32_t>(sample) * volume) / 100);
}

static esp_err_t create_i2s(uint32_t sample_rate, i2s_chan_handle_t *channel)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    esp_err_t result = i2s_new_channel(&channel_config, channel, nullptr);
    if (result != ESP_OK) {
        return result;
    }

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_LRCLK,
            .dout = AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    result = i2s_channel_init_std_mode(*channel, &std_config);
    if (result != ESP_OK) {
        i2s_del_channel(*channel);
        *channel = nullptr;
        return result;
    }
    result = i2s_channel_enable(*channel);
    if (result != ESP_OK) {
        i2s_del_channel(*channel);
        *channel = nullptr;
    }
    return result;
}

} // namespace

extern "C" esp_err_t mp3_play_file(const char *path, const volatile int *volume_percent,
                                    mp3_stop_callback_t stop_callback, void *stop_context)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *input = static_cast<uint8_t *>(heap_caps_malloc(INPUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int16_t *pcm = static_cast<int16_t *>(heap_caps_malloc(
        micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    int16_t *stereo = static_cast<int16_t *>(heap_caps_malloc(
        micro_mp3::MP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (input == nullptr || pcm == nullptr || stereo == nullptr) {
        free(input);
        free(pcm);
        free(stereo);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    micro_mp3::Mp3Decoder decoder;
    i2s_chan_handle_t i2s = nullptr;
    size_t bytes_available = 0;
    size_t offset = 0;
    bool eof = false;
    esp_err_t result = ESP_OK;

    while (true) {
        if (stop_callback != nullptr && stop_callback(stop_context)) {
            ESP_LOGI(TAG, "MP3 stopped by user");
            break;
        }
        if (offset == bytes_available && !eof) {
            bytes_available = fread(input, 1, INPUT_BYTES, file);
            offset = 0;
            eof = bytes_available == 0;
        }
        if (offset == bytes_available && eof) {
            break;
        }

        size_t consumed = 0;
        size_t samples = 0;
        const micro_mp3::Mp3Result decode_result = decoder.decode(
            input + offset, bytes_available - offset,
            reinterpret_cast<uint8_t *>(pcm), micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES,
            consumed, samples);
        offset += consumed;

        if (decode_result == micro_mp3::MP3_STREAM_INFO_READY && i2s == nullptr) {
            result = create_i2s(decoder.get_sample_rate(), &i2s);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(result));
                break;
            }
            ESP_LOGI(TAG, "MP3 stream: %d Hz, %d channel(s)", decoder.get_sample_rate(), decoder.get_channels());
            continue;
        }
        if (decode_result < 0 && decode_result != micro_mp3::MP3_DECODE_ERROR) {
            ESP_LOGE(TAG, "MP3 decoder failed: %d", static_cast<int>(decode_result));
            result = ESP_FAIL;
            break;
        }
        if (samples > 0) {
            if (i2s == nullptr) {
                result = create_i2s(decoder.get_sample_rate(), &i2s);
                if (result != ESP_OK) {
                    break;
                }
            }
            const int channels = decoder.get_channels();
            const int volume = volume_percent != nullptr ? *volume_percent : 100;
            for (size_t index = 0; index < samples; ++index) {
                const int16_t left = pcm[index * channels];
                const int16_t right = channels == 2 ? pcm[index * 2 + 1] : left;
                stereo[index * 2] = scale_sample(left, volume);
                stereo[index * 2 + 1] = scale_sample(right, volume);
            }
            size_t written = 0;
            result = i2s_channel_write(i2s, stereo, samples * 2 * sizeof(int16_t), &written, portMAX_DELAY);
            if (result != ESP_OK || written != samples * 2 * sizeof(int16_t)) {
                result = ESP_FAIL;
                break;
            }
        }
        if (consumed == 0 && decode_result == micro_mp3::MP3_NEED_MORE_DATA && offset < bytes_available) {
            // The decoder has retained the incomplete frame internally. Read a fresh chunk.
            offset = bytes_available;
        }
    }

    if (i2s != nullptr) {
        i2s_channel_disable(i2s);
        i2s_del_channel(i2s);
    }
    free(stereo);
    free(pcm);
    free(input);
    fclose(file);
    return result;
}
