/* QR Museum Lamp proof of concept for the JC3248W535 ESP32-S3 board. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "esp_lcd_axs15231b.h"

#define TAG "museum_lamp"

/* JC3248W535 board wiring, verified against the supplied vendor demo. */
#define LCD_HOST SPI2_HOST
#define LCD_CS 45
#define LCD_PCLK 47
#define LCD_DATA0 21
#define LCD_DATA1 48
#define LCD_DATA2 40
#define LCD_DATA3 39
#define LCD_BACKLIGHT 1
#define LCD_WIDTH 480
#define LCD_HEIGHT 320
#define LCD_DRAW_LINES 12

#define SD_CLK 12
#define SD_CMD 11
#define SD_D0 13
#define SD_MOUNT_POINT "/sdcard"

#define AUDIO_BCLK 42
#define AUDIO_LRCLK 2
#define AUDIO_DOUT 41

#define QR_UART UART_NUM_1
#define MAX_MEDIA_ENTRIES 48
#define MAX_QR_TEXT 96
#define MAX_PATH 112
#define MAX_TITLE 56

typedef struct {
    char code[MAX_QR_TEXT];
    char path[MAX_PATH];
    char title[MAX_TITLE];
} media_entry_t;

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t file_size;
    char wave[4];
} wav_riff_t;

typedef struct __attribute__((packed)) {
    char id[4];
    uint32_t size;
} wav_chunk_t;

typedef struct __attribute__((packed)) {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

static esp_lcd_panel_handle_t s_lcd;
static uint16_t *s_lcd_buffer;
static SemaphoreHandle_t s_screen_lock;
static QueueHandle_t s_play_queue;
static media_entry_t s_media[MAX_MEDIA_ENTRIES];
static size_t s_media_count;

static void trim(char *text)
{
    char *start = text;
    while (isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
}

#if 0 /* ESP32-2432S024 ILI9341 implementation retained only as migration reference. */
static void lcd_write(bool data, const void *buffer, size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    ESP_ERROR_CHECK(gpio_set_level(LCD_DC, data));
    spi_transaction_t transaction = {
        .length = bytes * 8,
        .tx_buffer = buffer,
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_lcd, &transaction));
}

static void lcd_command(uint8_t command, const uint8_t *data, size_t data_length)
{
    lcd_write(false, &command, 1);
    lcd_write(true, data, data_length);
}

static void lcd_init(void)
{
    const gpio_config_t dc_config = {
        .pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&dc_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 0));

    const spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = 12,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 20 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const spi_device_interface_config_t device = {
        .clock_speed_hz = 24 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &device, &s_lcd));

    /* ILI9341 sequence for the ESP32-2432S024 N/R/C variants. */
    static const uint8_t power_b[] = {0x00, 0xC1, 0x30};
    static const uint8_t power_seq[] = {0x64, 0x03, 0x12, 0x81};
    static const uint8_t driver_timing_a[] = {0x85, 0x00, 0x78};
    static const uint8_t driver_timing_b[] = {0x00, 0x00};
    static const uint8_t power_a[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    static const uint8_t pump[] = {0x20};
    static const uint8_t power1[] = {0x23};
    static const uint8_t power2[] = {0x10};
    static const uint8_t vcom1[] = {0x3E, 0x28};
    static const uint8_t vcom2[] = {0x86};
    static const uint8_t madctl[] = {0x48}; /* BGR + mirror X */
    static const uint8_t pixel_format[] = {0x55};
    static const uint8_t frame_rate[] = {0x00, 0x18};
    static const uint8_t display_func[] = {0x08, 0x82, 0x27};
    static const uint8_t gamma_enable[] = {0x00};
    static const uint8_t gamma_positive[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
    static const uint8_t gamma_negative[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};

    lcd_command(0x01, NULL, 0); /* software reset */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_command(0xCF, power_b, sizeof(power_b));
    lcd_command(0xED, power_seq, sizeof(power_seq));
    lcd_command(0xE8, driver_timing_a, sizeof(driver_timing_a));
    lcd_command(0xEA, driver_timing_b, sizeof(driver_timing_b));
    lcd_command(0xCB, power_a, sizeof(power_a));
    lcd_command(0xF7, pump, sizeof(pump));
    lcd_command(0xC0, power1, sizeof(power1));
    lcd_command(0xC1, power2, sizeof(power2));
    lcd_command(0xC5, vcom1, sizeof(vcom1));
    lcd_command(0xC7, vcom2, sizeof(vcom2));
    lcd_command(0x36, madctl, sizeof(madctl));
    lcd_command(0x3A, pixel_format, sizeof(pixel_format));
    lcd_command(0xB1, frame_rate, sizeof(frame_rate));
    lcd_command(0xB6, display_func, sizeof(display_func));
    lcd_command(0xF2, gamma_enable, sizeof(gamma_enable));
    lcd_command(0x26, gamma_enable, sizeof(gamma_enable));
    lcd_command(0xE0, gamma_positive, sizeof(gamma_positive));
    lcd_command(0xE1, gamma_negative, sizeof(gamma_negative));
    lcd_command(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_command(0x29, NULL, 0);
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 1));
}

static void lcd_set_window(int x, int y, int width, int height)
{
    const uint8_t columns[] = {x >> 8, x & 0xff, (x + width - 1) >> 8, (x + width - 1) & 0xff};
    const uint8_t pages[] = {y >> 8, y & 0xff, (y + height - 1) >> 8, (y + height - 1) & 0xff};
    lcd_command(0x2A, columns, sizeof(columns));
    lcd_command(0x2B, pages, sizeof(pages));
    lcd_command(0x2C, NULL, 0);
}

static void lcd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) { width = LCD_WIDTH - x; }
    if (y + height > LCD_HEIGHT) { height = LCD_HEIGHT - y; }

    uint16_t *line = heap_caps_malloc(width * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (line == NULL) {
        ESP_LOGE(TAG, "No DMA memory for display line");
        return;
    }
    const uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
    for (int index = 0; index < width; ++index) {
        line[index] = wire_color;
    }
    lcd_set_window(x, y, width, height);
    for (int row = 0; row < height; ++row) {
        lcd_write(true, line, width * sizeof(uint16_t));
    }
    free(line);
}
#endif

static void lcd_init(void)
{
    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 0));

    const spi_bus_config_t bus_config = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3,
        LCD_WIDTH * LCD_DRAW_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = NULL, /* Vendor driver supplies its proven AXS15231B defaults. */
        .init_cmds_size = 0,
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel_config, &s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd));
    /* The glass is 320x480 natively.  Rotate it for the 480x320 lamp UI. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_lcd, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_lcd, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd, true));

    s_lcd_buffer = heap_caps_malloc(LCD_WIDTH * LCD_DRAW_LINES * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_lcd_buffer == NULL) {
        ESP_LOGE(TAG, "No internal DMA memory for display buffer");
        abort();
    }
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 1));
}

static void lcd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) { width = LCD_WIDTH - x; }
    if (y + height > LCD_HEIGHT) { height = LCD_HEIGHT - y; }
    if (width <= 0 || height <= 0) {
        return;
    }

    const uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
    for (int row = 0; row < LCD_DRAW_LINES; ++row) {
        for (int column = 0; column < width; ++column) {
            s_lcd_buffer[row * width + column] = wire_color;
        }
    }
    for (int row = 0; row < height; row += LCD_DRAW_LINES) {
        const int transfer_lines = (height - row) < LCD_DRAW_LINES ? (height - row) : LCD_DRAW_LINES;
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd, x, y + row, x + width,
                                                   y + row + transfer_lines, s_lcd_buffer));
    }
}

/* Compact 3x5 uppercase font, packed row-wise (bit 14 is top-left). */
static uint16_t glyph(char input)
{
    const char c = (char)toupper((unsigned char)input);
    switch (c) {
    case 'A': return 0x57DA; case 'B': return 0xD79D; case 'C': return 0x71C7;
    case 'D': return 0xD6DD; case 'E': return 0xF3CF; case 'F': return 0xF3C8;
    case 'G': return 0x73DD; case 'H': return 0xB7ED; case 'I': return 0xE492;
    case 'J': return 0x249D; case 'K': return 0xBACE; case 'L': return 0x924F;
    case 'M': return 0xBFED; case 'N': return 0xBDEB; case 'O': return 0x56AD;
    case 'P': return 0xD7C8; case 'Q': return 0x56EF; case 'R': return 0xD7CA;
    case 'S': return 0x71C7; case 'T': return 0xE492; case 'U': return 0xB6AD;
    case 'V': return 0xB6A4; case 'W': return 0xB7FF; case 'X': return 0xB44B;
    case 'Y': return 0xB449; case 'Z': return 0x249F;
    case '0': return 0x56ED; case '1': return 0x6C92; case '2': return 0x249F;
    case '3': return 0x24E7; case '4': return 0x9DB2; case '5': return 0xF1C7;
    case '6': return 0x71ED; case '7': return 0x2492; case '8': return 0x56ED;
    case '9': return 0x56E7; case '-': return 0x01C0; case '.': return 0x0002;
    case ':': return 0x0402; case '/': return 0x1248; case ' ': return 0;
    default: return 0xE04E;
    }
}

static void lcd_text(int x, int y, const char *text, int scale, uint16_t foreground)
{
    for (; *text != '\0' && x + 3 * scale < LCD_WIDTH; ++text, x += 4 * scale) {
        const uint16_t bits = glyph(*text);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (bits & (1U << (14 - (row * 3 + column)))) {
                    lcd_fill_rect(x + column * scale, y + row * scale, scale, scale, foreground);
                }
            }
        }
    }
}

static void screen_message(uint16_t background, const char *line1, const char *line2)
{
    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, background);
    lcd_fill_rect(0, 0, LCD_WIDTH, 8, 0xFFFF);
    lcd_text(14, 68, line1, 4, 0xFFFF);
    lcd_text(14, 140, line2, 3, 0xFFFF);
    xSemaphoreGive(s_screen_lock);
}

static esp_err_t mount_sd_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
#endif
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t result = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (result == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
    }
    return result;
}

static void load_media_map(void)
{
    char map_path[128];
    snprintf(map_path, sizeof(map_path), "%s/media-map.csv", SD_MOUNT_POINT);
    FILE *file = fopen(map_path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", map_path);
        return;
    }

    char line[300];
    while (s_media_count < MAX_MEDIA_ENTRIES && fgets(line, sizeof(line), file) != NULL) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *code = strtok(line, ";");
        char *path = strtok(NULL, ";");
        char *title = strtok(NULL, "");
        if (code == NULL || path == NULL || title == NULL) {
            ESP_LOGW(TAG, "Ignoring malformed media-map row");
            continue;
        }
        trim(code);
        trim(path);
        trim(title);
        if (strstr(path, "..") != NULL || *code == '\0' || *path == '\0') {
            ESP_LOGW(TAG, "Ignoring unsafe media-map row");
            continue;
        }
        media_entry_t *entry = &s_media[s_media_count++];
        strlcpy(entry->code, code, sizeof(entry->code));
        if (path[0] == '/') {
            strlcpy(entry->path, path, sizeof(entry->path));
        } else {
            snprintf(entry->path, sizeof(entry->path), "%s/%s", SD_MOUNT_POINT, path);
        }
        strlcpy(entry->title, title, sizeof(entry->title));
    }
    fclose(file);
    ESP_LOGI(TAG, "Loaded %u media entries", (unsigned)s_media_count);
}

static const media_entry_t *find_media(const char *code)
{
    for (size_t index = 0; index < s_media_count; ++index) {
        if (strcmp(s_media[index].code, code) == 0) {
            return &s_media[index];
        }
    }
    return NULL;
}

static bool read_wav_header(FILE *file, wav_fmt_t *format, uint32_t *data_size)
{
    wav_riff_t riff;
    if (fread(&riff, sizeof(riff), 1, file) != 1 || memcmp(riff.riff, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4) != 0) {
        return false;
    }
    bool got_format = false;
    while (!feof(file)) {
        wav_chunk_t chunk;
        if (fread(&chunk, sizeof(chunk), 1, file) != 1) {
            break;
        }
        if (memcmp(chunk.id, "fmt ", 4) == 0) {
            if (chunk.size < sizeof(*format) || fread(format, sizeof(*format), 1, file) != 1) {
                return false;
            }
            if (chunk.size > sizeof(*format)) {
                fseek(file, chunk.size - sizeof(*format), SEEK_CUR);
            }
            got_format = true;
        } else if (memcmp(chunk.id, "data", 4) == 0 && got_format) {
            *data_size = chunk.size;
            return true;
        } else {
            fseek(file, chunk.size, SEEK_CUR);
        }
        if (chunk.size & 1U) {
            fseek(file, 1, SEEK_CUR);
        }
    }
    return false;
}

static void play_wav(const media_entry_t *entry)
{
    FILE *file = fopen(entry->path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open audio: %s", entry->path);
        screen_message(0xB000, "FILE ERROR", entry->title);
        return;
    }
    wav_fmt_t format = {0};
    uint32_t data_bytes = 0;
    if (!read_wav_header(file, &format, &data_bytes) || format.format != 1 ||
        (format.channels != 1 && format.channels != 2) ||
        (format.bits_per_sample != 8 && format.bits_per_sample != 16) ||
        format.sample_rate < 8000 || format.sample_rate > 48000) {
        ESP_LOGE(TAG, "Unsupported WAV: PCM, mono/stereo, 8/16-bit, 8-48kHz required");
        screen_message(0xB000, "WAV ERROR", "CHECK FORMAT");
        fclose(file);
        return;
    }

    screen_message(0x05A0, "PLAYING", entry->title);
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    i2s_chan_handle_t i2s = NULL;
    esp_err_t result = i2s_new_channel(&channel_config, &i2s, NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S allocation failed: %s", esp_err_to_name(result));
        fclose(file);
        return;
    }
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(format.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_LRCLK,
            .dout = AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    result = i2s_channel_init_std_mode(i2s, &std_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(result));
        i2s_del_channel(i2s);
        fclose(file);
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(i2s));

    uint8_t input[2048];
    int16_t output[1024 * 2];
    const int bytes_per_sample = format.bits_per_sample / 8;
    const int frame_bytes = format.channels * bytes_per_sample;
    uint32_t remaining = data_bytes;
    while (remaining > 0) {
        const size_t request = remaining < sizeof(input) ? remaining : sizeof(input);
        const size_t received = fread(input, 1, request - (request % frame_bytes), file);
        if (received < (size_t)frame_bytes) {
            break;
        }
        remaining -= received;
        size_t samples = 0;
        for (size_t offset = 0; offset + (size_t)frame_bytes <= received && samples < 1024; offset += frame_bytes) {
            int16_t left = format.bits_per_sample == 8
                ? (int16_t)(((int)input[offset] - 128) << 8)
                : (int16_t)(input[offset] | (input[offset + 1] << 8));
            int16_t right = left;
            if (format.channels == 2) {
                const size_t right_offset = offset + bytes_per_sample;
                right = format.bits_per_sample == 8
                    ? (int16_t)(((int)input[right_offset] - 128) << 8)
                    : (int16_t)(input[right_offset] | (input[right_offset + 1] << 8));
            }
            output[samples * 2] = (int16_t)((left * CONFIG_LAMP_AUDIO_VOLUME) / 100);
            output[samples * 2 + 1] = (int16_t)((right * CONFIG_LAMP_AUDIO_VOLUME) / 100);
            ++samples;
        }
        size_t written = 0;
        result = i2s_channel_write(i2s, output, samples * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        if (result != ESP_OK || written != samples * 2 * sizeof(int16_t)) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(result));
            break;
        }
    }
    ESP_ERROR_CHECK(i2s_channel_disable(i2s));
    ESP_ERROR_CHECK(i2s_del_channel(i2s));
    fclose(file);
    screen_message(0xD680, "SCAN A QR", "READY");
}

static void audio_task(void *argument)
{
    media_entry_t entry;
    while (true) {
        if (xQueueReceive(s_play_queue, &entry, portMAX_DELAY) == pdTRUE) {
            play_wav(&entry);
        }
    }
}

static void qr_task(void *argument)
{
    char code[MAX_QR_TEXT] = {0};
    size_t length = 0;
    while (true) {
        uint8_t byte;
        const int received = uart_read_bytes(QR_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (received == 1) {
            if (byte == '\r' || byte == '\n') {
                code[length] = '\0';
                trim(code);
                if (length > 0) {
                    ESP_LOGI(TAG, "QR: %s", code);
                    const media_entry_t *entry = find_media(code);
                    if (entry == NULL) {
                        screen_message(0xB000, "UNKNOWN QR", code);
                    } else if (xQueueSend(s_play_queue, entry, 0) != pdTRUE) {
                        screen_message(0xB000, "PLEASE WAIT", "AUDIO QUEUE FULL");
                    }
                }
                length = 0;
            } else if (isprint(byte) && length + 1 < sizeof(code)) {
                code[length++] = (char)byte;
            }
        }
    }
}

static void qr_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = CONFIG_LAMP_QR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(QR_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(QR_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(QR_UART, UART_PIN_NO_CHANGE, CONFIG_LAMP_QR_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    s_screen_lock = xSemaphoreCreateMutex();
    s_play_queue = xQueueCreate(2, sizeof(media_entry_t));
    if (s_screen_lock == NULL || s_play_queue == NULL) {
        ESP_LOGE(TAG, "Cannot allocate application resources");
        return;
    }

    lcd_init();
    screen_message(0xD680, "QR LAMP", "MOUNTING SD");
    const esp_err_t sd_result = mount_sd_card();
    if (sd_result != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(sd_result));
        screen_message(0xB000, "SD ERROR", "CHECK CARD");
        return;
    }
    load_media_map();
    if (s_media_count == 0) {
        screen_message(0xB000, "MAP ERROR", "CHECK SD CARD");
        return;
    }
    qr_uart_init();
    screen_message(0xD680, "SCAN A QR", "READY");
    xTaskCreate(audio_task, "audio", 6144, NULL, 5, NULL);
    xTaskCreate(qr_task, "qr_reader", 4096, NULL, 6, NULL);
}
