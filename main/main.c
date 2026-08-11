/* QR Museum Lamp proof of concept for the JC3248W535 ESP32-S3 board. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "avi_reader.h"
#include "cinepak_decoder.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_touch.h"
#include "jpeg_decoder.h"
#include "jpegdec_player.h"
#include "lv_port.h"
#include "mp3_player.h"
#include "new_jpeg_player.h"

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
#define LCD_PANEL_WIDTH 320
#define LCD_PANEL_HEIGHT 480
/* The physical glass is native 320x480. The lamp UI is landscape. */
#define LCD_WIDTH 480
#define LCD_HEIGHT 320
#define LCD_DRAW_LINES 12

/* AXS15231B capacitive touch controller, as wired on the JC3248W535. */
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_SDA 4
#define TOUCH_I2C_SPEED_HZ 400000

#define SD_CLK 12
#define SD_CMD 11
#define SD_D0 13
#define SD_MOUNT_POINT "/sdcard"
#define LOGO_PATH SD_MOUNT_POINT "/assets/logo.jpg"

#define AUDIO_BCLK 42
#define AUDIO_LRCLK 2
#define AUDIO_DOUT 41

#define QR_UART UART_NUM_1
#define MAX_MEDIA_ENTRIES 160
#define MAX_QR_TEXT 96
#define MAX_PATH 112
#define MAX_TITLE 56
#define MAX_MJPEG_FRAME_BYTES (384 * 1024)
#define MJPEG_READ_BUFFER_BYTES (32 * 1024)
#define MAX_INFO_IMAGE_BYTES (512 * 1024)
#define MAX_SHOW_SLIDES 64
#define MAX_SHOW_LINE 192
#define WAV_INPUT_BYTES 2048
#define WAV_OUTPUT_SAMPLES 1024
#define VIDEO_CONTENT_HEIGHT 272
#define VIDEO_CONTROL_HEIGHT (LCD_HEIGHT - VIDEO_CONTENT_HEIGHT)
#define QR_RETRIGGER_GUARD_US (2 * 1000 * 1000LL)
#define UNKNOWN_QR_MESSAGE_MS 5000
#define FILE_ERROR_HOME_DELAY_MS 5000
#define POST_PLAYBACK_HOME_DELAY_MS 1000
#define MAINTENANCE_CORNER_SIZE 80
#define MAINTENANCE_GESTURE_TIMEOUT_US (1500 * 1000LL)
#define DIRECT_PLAYER_STRIP_WIDTH 48
#define DIRECT_PLAYER_TRANSFER_TIMEOUT_MS 1000

typedef struct {
    char code[MAX_QR_TEXT];
    char path[MAX_PATH];
    char title[MAX_TITLE];
    uint8_t video_fps;
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

typedef struct {
    FILE *file;
    uint8_t *buffer;
    size_t length;
    size_t position;
} mjpeg_reader_t;

typedef struct {
    char path[MAX_PATH];
    TaskHandle_t owner;
    volatile bool stop_requested;
    volatile bool started;
    volatile bool finished;
    esp_err_t result;
} video_audio_context_t;

typedef struct {
    uint32_t start_ms;
    char image_path[MAX_PATH];
} show_slide_t;

typedef struct {
    char audio_path[MAX_PATH];
    show_slide_t slides[MAX_SHOW_SLIDES];
    size_t slide_count;
} slideshow_t;

static esp_lcd_panel_handle_t s_lcd;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_touch_handle_t s_touch;
static lv_disp_t *s_lvgl_display;
static lv_obj_t *s_volume_label;
static lv_img_dsc_t s_logo_image;
static uint16_t *s_lcd_buffer;
static uint16_t *s_framebuffer;
static SemaphoreHandle_t s_screen_lock;
static QueueHandle_t s_play_queue;
static media_entry_t s_media[MAX_MEDIA_ENTRIES];
static size_t s_media_count;
static volatile int s_audio_volume = CONFIG_LAMP_AUDIO_VOLUME;
static volatile bool s_media_playing;
static volatile bool s_volume_redraw_pending;
static volatile int64_t s_qr_ignore_until_us;
static sdmmc_card_t *s_sd_card;
static tinyusb_msc_storage_handle_t s_usb_storage;
static volatile bool s_usb_msc_active;
static volatile bool s_usb_error_reset_pending;
static volatile bool s_maintenance_menu_active;
static SemaphoreHandle_t s_direct_player_done;

static void update_volume_label(void);
static void adjust_audio_volume(int delta);
static bool wait_for_lcd_transfers(void);
static void info_narration_task(void *argument);
static void start_usb_msc_mode(void);
static void show_usb_error(void);
static void show_maintenance_menu(void);
static bool slideshow_mp3_stop(void *argument);

/* USB Mass Storage descriptor.  It is only installed after the user enters
 * maintenance mode, so the normal USB Serial/JTAG flash connection remains
 * available while the lamp is running. */
#define USB_MSC_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    USB_MSC_ITF = 0,
    USB_MSC_ITF_COUNT,
};

static tusb_desc_device_t s_usb_msc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_usb_msc_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_MSC_ITF_COUNT, 0, USB_MSC_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(USB_MSC_ITF, 0, 0x01, 0x81, 64),
};

static const char *s_usb_msc_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Gelders Smalspoormuseum",
    "QR Museum Lamp SD",
    "GSS-LAMP",
};

/* Exact JC3248W535 initialization sequence from the supplied DEMO_MJPEG BSP. */
static const axs15231b_lcd_init_cmd_t s_jc3248w535_init_cmds[] = {
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t []){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x2C, (uint8_t []){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

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

static void lcd_panel_init(void)
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

static void lcd_panel_init(void)
{
    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 0));

    const spi_bus_config_t bus_config = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3,
        LCD_PANEL_WIDTH * LCD_PANEL_HEIGHT * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));
    s_lcd_io = io;

    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = s_jc3248w535_init_cmds,
        .init_cmds_size = sizeof(s_jc3248w535_init_cmds) / sizeof(s_jc3248w535_init_cmds[0]),
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
    /* ESP-IDF's argument is `off`: false turns the panel on. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd, false));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 1));

}

static void lcd_init(void)
{
    lcd_panel_init();

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = -1;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd,
        /* The official DEMO_LVGL uses a full logical framebuffer.  Its
         * rotation routine then splits the 480x320 frame into ten complete
         * 48x320 strips for QSPI, preserving every glyph pixel. */
        .buffer_size = LCD_WIDTH * LCD_HEIGHT,
        /* Match the vendor LVGL port: one complete 32 x 480 rotated strip.
         * Splitting it across multiple RAMWRC transactions corrupts detail. */
        .trans_size = LCD_PANEL_WIDTH * LCD_PANEL_HEIGHT / 10,
        /* Rotate the complete landscape UI 180 degrees so the board's
         * connectors point toward the lamp base. */
        .sw_rotate = LV_DISP_ROT_270,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .flags = { .buff_dma = false, .buff_spiram = true },
    };
    s_lvgl_display = lvgl_port_add_disp(&display_cfg);
    if (s_lvgl_display == NULL) {
        ESP_LOGE(TAG, "Cannot create LVGL display");
        abort();
    }
}

static void touch_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    io_config.scl_speed_hz = TOUCH_I2C_SPEED_HZ;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &touch_io));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_PANEL_WIDTH,
        .y_max = LCD_PANEL_HEIGHT,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_axs15231b(touch_io, &touch_config, &s_touch));
    ESP_LOGI(TAG, "Touch enabled: tap during video stops playback");
}

static bool touch_get_logical_point(uint16_t *logical_x, uint16_t *logical_y)
{
    if (s_touch == NULL || esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        return false;
    }
    uint16_t physical_x = 0;
    uint16_t physical_y = 0;
    uint8_t points = 0;
    if (!esp_lcd_touch_get_coordinates(s_touch, &physical_x, &physical_y, NULL, &points, 1) || points == 0) {
        return false;
    }
    /* Inverse mapping for the 180-degree-rotated landscape UI. */
    if (logical_x != NULL) {
        *logical_x = LCD_WIDTH - 1 - physical_y;
    }
    if (logical_y != NULL) {
        *logical_y = physical_x;
    }
    return true;
}

static bool touch_is_pressed(void)
{
    return touch_get_logical_point(NULL, NULL);
}

static bool stop_media_on_touch(void *context)
{
    (void)context;
    static bool was_pressed;
    uint16_t x = 0;
    uint16_t y = 0;
    if (!touch_get_logical_point(&x, &y)) {
        was_pressed = false;
        return false;
    }
    if (was_pressed) {
        return false;
    }
    was_pressed = true;
    if (y >= LCD_HEIGHT - 70) {
        adjust_audio_volume(x < LCD_WIDTH / 2 ? -10 : 10);
        return false;
    }
    return true;
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

    /* Same 90-degree software rotation as the vendor LVGL port, in the
     * physical landscape orientation of this board. */
    const int panel_x = y;
    const int panel_y = LCD_WIDTH - (x + width);
    const int panel_width = height;
    const int panel_height = width;
    const uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
    for (int row = 0; row < panel_height; ++row) {
        for (int column = 0; column < panel_width; ++column) {
            s_framebuffer[(panel_y + row) * LCD_PANEL_WIDTH + panel_x + column] = wire_color;
        }
    }
}

static void lcd_flush_framebuffer(void)
{
    for (int row = 0; row < LCD_PANEL_HEIGHT; row += LCD_DRAW_LINES) {
        const int transfer_lines = (LCD_PANEL_HEIGHT - row) < LCD_DRAW_LINES
            ? (LCD_PANEL_HEIGHT - row) : LCD_DRAW_LINES;
        memcpy(s_lcd_buffer, &s_framebuffer[row * LCD_PANEL_WIDTH],
               LCD_PANEL_WIDTH * transfer_lines * sizeof(uint16_t));
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd, 0, row,
                                                   LCD_PANEL_WIDTH, row + transfer_lines,
                                                   s_lcd_buffer));
    }
}

/* Standard 8x8 uppercase glyphs: denser and easier to read than the
 * deliberately coarse diagnostic 5x7 font used during panel bring-up. */
static uint8_t glyph_row(char input, int row)
{
    const char c = (char)toupper((unsigned char)input);
    static const uint8_t font[][8] = {
        ['A'] = {0x3C,0x66,0xC3,0xC3,0xFF,0xC3,0xC3,0}, ['B'] = {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0},
        ['C'] = {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0}, ['D'] = {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0},
        ['E'] = {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0}, ['F'] = {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0},
        ['G'] = {0x3C,0x66,0xC0,0xDE,0xC6,0x66,0x3E,0}, ['H'] = {0xC3,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0},
        ['I'] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0}, ['J'] = {0x1F,0x06,0x06,0x06,0xC6,0xC6,0x7C,0},
        ['K'] = {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0}, ['L'] = {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0},
        ['M'] = {0xC3,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0}, ['N'] = {0xC3,0xE3,0xF3,0xFB,0xDF,0xCF,0xC7,0},
        ['O'] = {0x3C,0x66,0xC3,0xC3,0xC3,0x66,0x3C,0}, ['P'] = {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0},
        ['Q'] = {0x3C,0x66,0xC3,0xC3,0xDB,0x66,0x3D,0}, ['R'] = {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0},
        ['S'] = {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0}, ['T'] = {0xFF,0x18,0x18,0x18,0x18,0x18,0x3C,0},
        ['U'] = {0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0}, ['V'] = {0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x18,0},
        ['W'] = {0xC3,0xC3,0xC3,0xDB,0xFF,0xE7,0xC3,0}, ['X'] = {0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0},
        ['Y'] = {0xC3,0x66,0x3C,0x18,0x18,0x18,0x3C,0}, ['Z'] = {0xFF,0x86,0x0C,0x18,0x30,0x61,0xFF,0},
        ['0'] = {0x3C,0x66,0xCF,0xDB,0xF3,0x66,0x3C,0}, ['1'] = {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
        ['2'] = {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0}, ['3'] = {0x7E,0x0C,0x18,0x3C,0x06,0x66,0x3C,0},
        ['4'] = {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0}, ['5'] = {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},
        ['6'] = {0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0}, ['7'] = {0x7E,0x66,0x0C,0x18,0x30,0x30,0x30,0},
        ['8'] = {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0}, ['9'] = {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x78,0},
        ['-'] = {0,0,0,0x7E,0,0,0,0}, ['+'] = {0,0x18,0x18,0x7E,0x18,0x18,0}, ['.'] = {0,0,0,0,0,0x18,0x18,0},
        [':'] = {0,0x18,0x18,0,0x18,0x18,0,0}, ['/'] = {0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0},
    };
    if (row < 0 || row >= 8 || c == ' ') {
        return 0;
    }
    if ((unsigned char)c < sizeof(font) / sizeof(font[0])) {
        return font[(unsigned char)c][row];
    }
    return (uint8_t)(row == 0 || row == 4 || row == 7 ? 0xFF : 0x81);
}

static void lcd_text(int x, int y, const char *text, int scale, uint16_t foreground)
{
    for (; *text != '\0' && x + 8 * scale < LCD_WIDTH; ++text, x += 9 * scale) {
        for (int row = 0; row < 8; ++row) {
            const uint8_t bits = glyph_row(*text, row);
            for (int column = 0; column < 8; ++column) {
                if (bits & (1U << (7 - column))) {
                    lcd_fill_rect(x + column * scale, y + row * scale, scale, scale, foreground);
                }
            }
        }
    }
}

static void screen_message(uint16_t background, const char *line1, const char *line2)
{
    (void)background;
    /* A zero-timeout lock occasionally skipped this redraw directly after
     * video playback, leaving the last video frame visible. */
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    const bool show_logo = strcmp(line1, "SCAN EEN QR") == 0 && s_logo_image.data != NULL;
    const lv_color_t foreground = show_logo ? lv_color_black() : lv_color_white();
    lv_obj_set_style_bg_color(screen, show_logo ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    if (show_logo) {
        lv_obj_t *logo = lv_img_create(screen);
        lv_img_set_src(logo, &s_logo_image);
        lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 14);
    }

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, line1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, 440);
    lv_obj_set_style_text_color(title, foreground, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, show_logo ? 132 : 72);

    lv_obj_t *detail = lv_label_create(screen);
    lv_label_set_text(detail, line2);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(detail, 440);
    lv_obj_set_style_text_color(detail, foreground, LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, show_logo ? 184 : 174);

    s_volume_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_volume_label, foreground, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_volume_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_volume_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text_fmt(s_volume_label, "-    VOLUME %d%%    +", s_audio_volume);
    lv_obj_align(s_volume_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    if (show_logo && CONFIG_LAMP_FIRMWARE_VERSION[0] != '\0') {
        lv_obj_t *version = lv_label_create(screen);
        lv_label_set_text(version, CONFIG_LAMP_FIRMWARE_VERSION);
        lv_label_set_long_mode(version, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(version, lv_color_hex(0x667085), LV_PART_MAIN);
        lv_obj_set_style_text_font(version, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_width(version, 100);
        lv_obj_align(version, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    }

    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    lvgl_port_unlock();
}

static void load_logo(void)
{
    FILE *file = fopen(LOGO_PATH, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "Museum logo not found: %s", LOGO_PATH);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    const long file_size = ftell(file);
    rewind(file);
    /* High-quality JPEGs can be relatively large even when their decoded
     * dimensions fit the 480x320 display. Keep the compressed file in PSRAM. */
    if (file_size <= 0 || file_size > 1024 * 1024) {
        ESP_LOGW(TAG, "Museum logo has invalid size: %ld", file_size);
        fclose(file);
        return;
    }

    uint8_t *encoded = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (encoded == NULL || fread(encoded, 1, file_size, file) != (size_t)file_size) {
        ESP_LOGW(TAG, "Cannot read museum logo");
        free(encoded);
        fclose(file);
        return;
    }
    fclose(file);

    esp_jpeg_image_cfg_t config = {
        .indata = encoded,
        .indata_size = file_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK || info.width > LCD_WIDTH ||
        info.height > LCD_HEIGHT || info.output_len == 0) {
        ESP_LOGW(TAG, "Museum logo is not a usable JPEG");
        free(encoded);
        return;
    }

    uint8_t *pixels = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGW(TAG, "Not enough PSRAM for museum logo");
        free(encoded);
        return;
    }
    config.outbuf = pixels;
    config.outbuf_size = info.output_len;
    config.flags.swap_color_bytes = 1;
    esp_jpeg_image_output_t output = {0};
    if (esp_jpeg_decode(&config, &output) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot decode museum logo");
        free(pixels);
        free(encoded);
        return;
    }
    free(encoded);

    memset(&s_logo_image, 0, sizeof(s_logo_image));
    s_logo_image.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_logo_image.header.w = output.width;
    s_logo_image.header.h = output.height;
    s_logo_image.data_size = output.output_len;
    s_logo_image.data = pixels;
    ESP_LOGI(TAG, "Museum logo loaded: %ux%u", output.width, output.height);
}

/* Collection cards are generated by tools/build-collection.py as 480x320
 * JPEGs.  Decoding them locally keeps the lamp fully offline in the museum. */
static bool load_info_image(const char *path, lv_img_dsc_t *image, uint8_t **pixels_out)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long file_size = ftell(file);
    rewind(file);
    if (file_size <= 0 || file_size > MAX_INFO_IMAGE_BYTES) {
        fclose(file);
        return false;
    }
    uint8_t *encoded = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (encoded == NULL || fread(encoded, 1, file_size, file) != (size_t)file_size) {
        free(encoded);
        fclose(file);
        return false;
    }
    fclose(file);

    esp_jpeg_image_cfg_t config = {
        .indata = encoded,
        .indata_size = file_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 1 },
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK ||
        info.width != LCD_WIDTH || info.height != LCD_HEIGHT || info.output_len == 0) {
        free(encoded);
        return false;
    }
    uint8_t *pixels = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        free(encoded);
        return false;
    }
    config.outbuf = pixels;
    config.outbuf_size = info.output_len;
    esp_jpeg_image_output_t output = {0};
    const esp_err_t decode_result = esp_jpeg_decode(&config, &output);
    free(encoded);
    if (decode_result != ESP_OK) {
        free(pixels);
        return false;
    }
    memset(image, 0, sizeof(*image));
    image->header.cf = LV_IMG_CF_TRUE_COLOR;
    image->header.w = output.width;
    image->header.h = output.height;
    image->data_size = output.output_len;
    image->data = pixels;
    *pixels_out = pixels;
    return true;
}

static void play_info_page(const media_entry_t *entry)
{
    lv_img_dsc_t image = {0};
    uint8_t *pixels = NULL;
    if (!load_info_image(entry->path, &image, &pixels)) {
        screen_message(0xB000, "INFO ERROR", entry->title);
        vTaskDelay(pdMS_TO_TICKS(1200));
        screen_message(0xD680, "SCAN EEN QR", "GEREED");
        return;
    }
    if (!lvgl_port_lock(portMAX_DELAY)) {
        free(pixels);
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *card = lv_img_create(screen);
    lv_img_set_src(card, &image);
    lv_obj_center(card);
    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    lvgl_port_unlock();

    /* A narration is optional.  The TTS workflow only has to place
     * audio/gss-xxx.mp3 on the SD card; existing image-only cards keep
     * behaving exactly as before. */
    video_audio_context_t narration = {0};
    const int narration_path_length = snprintf(narration.path, sizeof(narration.path),
                                               "%s/audio/%s.mp3", SD_MOUNT_POINT,
                                               entry->code);
    bool narration_started = false;
    if (narration_path_length > 0 && (size_t)narration_path_length < sizeof(narration.path)) {
        FILE *narration_file = fopen(narration.path, "rb");
        if (narration_file != NULL) {
            fclose(narration_file);
            narration_started = xTaskCreate(info_narration_task, "info_audio", 6144,
                                             &narration, 6, NULL) == pdPASS;
            if (!narration_started) {
                ESP_LOGW(TAG, "Cannot start narration task: %s", narration.path);
            }
        }
    }

    /* Wait for a new tap, or let an attached narration finish. Consume a
     * pressed state first so a long tap never opens and closes the card in
     * one gesture. */
    while (touch_get_logical_point(NULL, NULL)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bool narration_completed = false;
    while (true) {
        if (narration_started && narration.finished) {
            narration_completed = true;
            break;
        }
        if (touch_get_logical_point(NULL, NULL)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (!narration_completed && touch_get_logical_point(NULL, NULL)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (narration_completed) {
        vTaskDelay(pdMS_TO_TICKS(POST_PLAYBACK_HOME_DELAY_MS));
    }
    if (narration_started) {
        narration.stop_requested = true;
        while (!narration.finished) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
    wait_for_lcd_transfers();
    free(pixels);
}

static void update_volume_label(void)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (s_volume_label != NULL) {
        lv_label_set_text_fmt(s_volume_label, "-    VOLUME %d%%    +", s_audio_volume);
        lv_obj_invalidate(s_volume_label);
    }
    lvgl_port_unlock();
}

static void adjust_audio_volume(int delta)
{
    int next = s_audio_volume + delta;
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    if (next != s_audio_volume) {
        s_audio_volume = next;
        s_volume_redraw_pending = true;
        ESP_LOGI(TAG, "Audio volume: %d%%", next);
        update_volume_label();
    }
}

#if 0
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
    lcd_flush_framebuffer();
    xSemaphoreGive(s_screen_lock);
}
#endif

static esp_err_t mount_sd_card_at_frequency(int max_freq_khz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = max_freq_khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    s_sd_card = NULL;
    esp_err_t result = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_sd_card);
    if (result == ESP_OK) {
        sdmmc_card_print_info(stdout, s_sd_card);
    }
    return result;
}

static esp_err_t mount_sd_card(void)
{
    static const int frequencies[] = { 20000, 10000, 5000 };
    esp_err_t result = ESP_FAIL;
    for (size_t attempt = 0; attempt < sizeof(frequencies) / sizeof(frequencies[0]); ++attempt) {
        result = mount_sd_card_at_frequency(frequencies[attempt]);
        if (result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD mount attempt %u at %d kHz failed: %s", (unsigned)(attempt + 1),
                 frequencies[attempt], esp_err_to_name(result));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return result;
}

/* `esp_vfs_fat_sdcard_unmount()` releases the normal lamp mount and also
 * frees its card descriptor.  MSC therefore gets a freshly initialised raw
 * SDMMC card, which it owns until the user resets the lamp. */
static esp_err_t init_raw_sd_card_for_usb(sdmmc_card_t **card_out)
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
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = host.init();
    if (result == ESP_OK) {
        result = sdmmc_host_init_slot(host.slot, &slot);
    }
    if (result == ESP_OK) {
        result = sdmmc_card_init(&host, card);
    }
    if (result != ESP_OK) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            host.deinit_p(host.slot);
        } else {
            host.deinit();
        }
        free(card);
        return result;
    }
    *card_out = card;
    return ESP_OK;
}

static void start_usb_msc_mode(void)
{
    if (s_usb_msc_active) {
        return;
    }
    /* Prevent a QR scan from queuing filesystem work while ownership changes. */
    s_usb_msc_active = true;
    s_usb_error_reset_pending = false;
    screen_message(0x05A0, "USB SD MODE", "KAART LOSKOPPELEN");
    wait_for_lcd_transfers();

    if (s_sd_card == NULL || esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sd_card) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot unmount SD card for USB MSC");
        show_usb_error();
        return;
    }
    s_sd_card = NULL;

    sdmmc_card_t *usb_card = NULL;
    esp_err_t result = init_raw_sd_card_for_usb(&usb_card);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot initialise SD card for USB MSC: %s", esp_err_to_name(result));
        show_usb_error();
        return;
    }

    const tinyusb_msc_storage_config_t storage_config = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config = {
                .max_files = 4,
                .format_if_mount_failed = false,
            },
            .format_flags = 0,
        },
        .medium.card = usb_card,
    };
    result = tinyusb_msc_new_storage_sdmmc(&storage_config, &s_usb_storage);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create USB MSC storage: %s", esp_err_to_name(result));
        show_usb_error();
        return;
    }

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    usb_config.descriptor.device = &s_usb_msc_device_descriptor;
    usb_config.descriptor.full_speed_config = s_usb_msc_config_descriptor;
    usb_config.descriptor.string = s_usb_msc_string_descriptors;
    usb_config.descriptor.string_count = sizeof(s_usb_msc_string_descriptors) / sizeof(s_usb_msc_string_descriptors[0]);
    result = tinyusb_driver_install(&usb_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start USB MSC: %s", esp_err_to_name(result));
        show_usb_error();
        return;
    }

    ESP_LOGI(TAG, "SD card is now exposed as USB Mass Storage");
    screen_message(0x05A0, "SD VIA USB", "WERP UIT - RESET");
}

static void show_usb_error(void)
{
    s_usb_error_reset_pending = true;
    screen_message(0xB000, "USB SD ERROR", "TIK VOOR RESET");
}

static void show_maintenance_menu(void)
{
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *usb = lv_label_create(screen);
    lv_label_set_text(usb, "USB-PC CONNECTIE");
    lv_obj_set_width(usb, 440);
    lv_obj_set_style_text_color(usb, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(usb, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_align(usb, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(usb, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *usb_detail = lv_label_create(screen);
    lv_label_set_text(usb_detail, "TIK BOVENSTE HELFT");
    lv_obj_set_width(usb_detail, 440);
    lv_obj_set_style_text_color(usb_detail, lv_color_hex(0x6FA8DC), LV_PART_MAIN);
    lv_obj_set_style_text_font(usb_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(usb_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(usb_detail, LV_ALIGN_TOP_MID, 0, 105);

    lv_obj_t *divider = lv_obj_create(screen);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 400, 2);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x0066CC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(divider, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *reset = lv_label_create(screen);
    lv_label_set_text(reset, "RESET MODULE");
    lv_obj_set_width(reset, 440);
    lv_obj_set_style_text_color(reset, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_align(reset, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(reset, LV_ALIGN_TOP_MID, 0, 205);

    lv_obj_t *reset_detail = lv_label_create(screen);
    lv_label_set_text(reset_detail, "TIK ONDERSTE HELFT");
    lv_obj_set_width(reset_detail, 440);
    lv_obj_set_style_text_color(reset_detail, lv_color_hex(0x6FA8DC), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(reset_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(reset_detail, LV_ALIGN_TOP_MID, 0, 248);
    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    lvgl_port_unlock();
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
        char *title = strtok(NULL, ";");
        char *fps_text = strtok(NULL, ";");
        if (code == NULL || path == NULL || title == NULL) {
            ESP_LOGW(TAG, "Ignoring malformed media-map row");
            continue;
        }
        trim(code);
        trim(path);
        trim(title);
        uint8_t video_fps = 0;
        if (fps_text != NULL) {
            trim(fps_text);
            if (*fps_text != '\0') {
                char *end = NULL;
                const long parsed_fps = strtol(fps_text, &end, 10);
                while (end != NULL && isspace((unsigned char)*end)) {
                    ++end;
                }
                if (end != fps_text && *end == '\0' && parsed_fps >= 1 && parsed_fps <= 30) {
                    video_fps = (uint8_t)parsed_fps;
                } else {
                    ESP_LOGW(TAG, "Ignoring invalid FPS '%s' for QR '%s'", fps_text, code);
                }
            }
        }
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
        entry->video_fps = video_fps;
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

static bool path_has_extension(const char *path, const char *extension)
{
    const char *suffix = strrchr(path, '.');
    return suffix != NULL && strcasecmp(suffix, extension) == 0;
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

static uint32_t wav_duration_ms(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    wav_fmt_t format = {0};
    uint32_t data_size = 0;
    const bool valid = read_wav_header(file, &format, &data_size) && format.byte_rate != 0;
    fclose(file);
    if (!valid) {
        return 0;
    }
    const uint64_t duration = ((uint64_t)data_size * 1000U) / format.byte_rate;
    return duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
}

static esp_err_t play_wav_file(const char *path, volatile bool *stop_requested,
                               video_audio_context_t *video_audio)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    wav_fmt_t format = {0};
    uint32_t data_bytes = 0;
    if (!read_wav_header(file, &format, &data_bytes) || format.format != 1 ||
        (format.channels != 1 && format.channels != 2) ||
        (format.bits_per_sample != 8 && format.bits_per_sample != 16) ||
        format.sample_rate < 8000 || format.sample_rate > 48000) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Keep the sizeable PCM buffers out of the playback task stack. */
    uint8_t *input = heap_caps_malloc(WAV_INPUT_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *output = heap_caps_malloc(WAV_OUTPUT_SAMPLES * 2 * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    i2s_chan_handle_t i2s = NULL;
    esp_err_t result = i2s_new_channel(&channel_config, &i2s, NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S allocation failed: %s", esp_err_to_name(result));
        free(input);
        free(output);
        fclose(file);
        return result;
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
        free(input);
        free(output);
        fclose(file);
        return result;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(i2s));

    const int bytes_per_sample = format.bits_per_sample / 8;
    const int frame_bytes = format.channels * bytes_per_sample;
    uint32_t remaining = data_bytes;
    while (remaining > 0) {
        if (stop_requested != NULL && *stop_requested) {
            break;
        }
        const size_t request = remaining < WAV_INPUT_BYTES ? remaining : WAV_INPUT_BYTES;
        const size_t received = fread(input, 1, request - (request % frame_bytes), file);
        if (received < (size_t)frame_bytes) {
            break;
        }
        remaining -= received;
        size_t samples = 0;
        for (size_t offset = 0; offset + (size_t)frame_bytes <= received && samples < WAV_OUTPUT_SAMPLES; offset += frame_bytes) {
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
            output[samples * 2] = (int16_t)((left * s_audio_volume) / 100);
            output[samples * 2 + 1] = (int16_t)((right * s_audio_volume) / 100);
            ++samples;
        }
        size_t written = 0;
        result = i2s_channel_write(i2s, output, samples * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        if (result != ESP_OK || written != samples * 2 * sizeof(int16_t)) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(result));
            break;
        }
        if (video_audio != NULL && !video_audio->started) {
            video_audio->started = true;
            xTaskNotifyGive(video_audio->owner);
        }
    }
    ESP_ERROR_CHECK(i2s_channel_disable(i2s));
    ESP_ERROR_CHECK(i2s_del_channel(i2s));
    free(input);
    free(output);
    fclose(file);
    return result;
}

static void play_wav(const media_entry_t *entry)
{
    screen_message(0x05A0, "PLAYING", entry->title);
    const esp_err_t result = play_wav_file(entry->path, NULL, NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "WAV playback failed: %s", esp_err_to_name(result));
        screen_message(0xB000, "WAV ERROR", "CHECK FORMAT");
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
}

static void video_audio_task(void *argument)
{
    video_audio_context_t *context = argument;
    if (path_has_extension(context->path, ".wav")) {
        context->result = play_wav_file(context->path, &context->stop_requested, context);
    } else if (path_has_extension(context->path, ".mp3")) {
        /* MP3 decoding has no start callback, so synchronise it immediately
         * before entering the decoder, just as slideshow MP3 audio does. */
        context->started = true;
        xTaskNotifyGive(context->owner);
        context->result = mp3_play_file(context->path, &s_audio_volume,
                                        slideshow_mp3_stop, context);
    } else {
        context->result = ESP_ERR_NOT_SUPPORTED;
    }
    context->finished = true;
    xTaskNotifyGive(context->owner);
    vTaskDelete(NULL);
}

static bool slideshow_mp3_stop(void *argument)
{
    video_audio_context_t *context = argument;
    return context->stop_requested;
}

static void info_narration_task(void *argument)
{
    video_audio_context_t *context = argument;
    context->result = mp3_play_file(context->path, &s_audio_volume,
                                    slideshow_mp3_stop, context);
    if (context->result != ESP_OK && !context->stop_requested) {
        ESP_LOGE(TAG, "Narration playback failed: %s", esp_err_to_name(context->result));
    }
    context->finished = true;
    vTaskDelete(NULL);
}

static void slideshow_audio_task(void *argument)
{
    video_audio_context_t *context = argument;
    if (path_has_extension(context->path, ".wav")) {
        context->result = play_wav_file(context->path, &context->stop_requested, context);
    } else if (path_has_extension(context->path, ".mp3")) {
        /* MP3 decoding reports no start callback.  Signal immediately before
         * entering it; slide timing is still anchored to this same task. */
        context->started = true;
        xTaskNotifyGive(context->owner);
        context->result = mp3_play_file(context->path, &s_audio_volume,
                                        slideshow_mp3_stop, context);
    } else {
        context->result = ESP_ERR_NOT_SUPPORTED;
    }
    context->finished = true;
    xTaskNotifyGive(context->owner);
    vTaskDelete(NULL);
}

static inline int mjpeg_reader_getc(mjpeg_reader_t *reader)
{
    if (reader->position == reader->length) {
        reader->length = fread(reader->buffer, 1, MJPEG_READ_BUFFER_BYTES, reader->file);
        reader->position = 0;
        if (reader->length == 0) {
            return EOF;
        }
    }
    return reader->buffer[reader->position++];
}

static bool read_next_mjpeg_frame(mjpeg_reader_t *reader, uint8_t *frame, size_t capacity, size_t *frame_size)
{
    int previous = -1;
    bool in_frame = false;
    *frame_size = 0;
    for (int value; (value = mjpeg_reader_getc(reader)) != EOF; previous = value) {
        if (!in_frame) {
            if (previous == 0xFF && value == 0xD8) {
                frame[0] = 0xFF;
                frame[1] = 0xD8;
                *frame_size = 2;
                in_frame = true;
            }
            continue;
        }
        if (*frame_size == capacity) {
            ESP_LOGE(TAG, "MJPEG frame exceeds %u bytes", (unsigned)capacity);
            return false;
        }
        frame[(*frame_size)++] = (uint8_t)value;
        if (previous == 0xFF && value == 0xD9) {
            return true;
        }
    }
    return false;
}

/*
 * Experimental player: this deliberately starts before LVGL is initialised.
 * It uses the AXS15231B panel's native 320x480 coordinates and performs the
 * same 90-degree strip transform as the vendor LVGL port.  Unlike the normal
 * player, no LVGL framebuffer is composed or flushed for each frame.
 */
static bool direct_player_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_awoken);
    return task_awoken == pdTRUE;
}

static uint16_t *s_direct_player_decoded;
static int s_direct_player_source_width;
static int s_direct_player_source_height;

static esp_err_t direct_player_draw_frame(const uint16_t *decoded, int decoded_width,
                                          int decoded_height, uint16_t *strip)
{
    const bool is_exact_half_scale = decoded_width * 2 == LCD_WIDTH &&
                                     decoded_height * 2 == VIDEO_CONTENT_HEIGHT;
    /* The AXS15231B QSPI transport advances its RAM write position between
     * strips.  ROT_270 therefore has to submit the rightmost logical strip
     * first, matching lvgl_port_flush_callback(). */
    for (int source_x = LCD_WIDTH - DIRECT_PLAYER_STRIP_WIDTH; source_x >= 0;
         source_x -= DIRECT_PLAYER_STRIP_WIDTH) {
        const int strip_width = (LCD_WIDTH - source_x) < DIRECT_PLAYER_STRIP_WIDTH
            ? (LCD_WIDTH - source_x) : DIRECT_PLAYER_STRIP_WIDTH;
        for (int y = 0; y < LCD_HEIGHT; ++y) {
            const int source_y = is_exact_half_scale ? y >> 1 :
                y * decoded_height / VIDEO_CONTENT_HEIGHT;
            const uint16_t *source = y < VIDEO_CONTENT_HEIGHT
                ? &decoded[source_y * decoded_width] : NULL;
            for (int x = 0; x < strip_width; ++x) {
                const int destination_x = source_x + x;
                const int source_x_scaled = is_exact_half_scale ? destination_x >> 1 :
                    destination_x * decoded_width / LCD_WIDTH;
                /* Compose a complete logical 480x320 screen.  The 48 lines
                 * after the video are explicitly cleared for the controls. */
                strip[(strip_width - 1 - x) * LCD_HEIGHT + y] =
                    source != NULL ? source[source_x_scaled] : 0;
            }
        }
        const esp_err_t result = esp_lcd_panel_draw_bitmap(
            /* Full-screen LV_DISP_ROT_270 mapping used by the UI. */
            s_lcd, 0, LCD_WIDTH - source_x - strip_width,
            LCD_HEIGHT, LCD_WIDTH - source_x, strip);
        if (result != ESP_OK) {
            return result;
        }
        if (xSemaphoreTake(s_direct_player_done,
                           pdMS_TO_TICKS(DIRECT_PLAYER_TRANSFER_TIMEOUT_MS)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

/* Normal playback shares the panel with LVGL.  Transfer a complete logical
 * 480x320 screen for every frame, just like the verified direct-player POC.
 * On this 90-degree-rotated panel, sending only the 272-pixel video viewport
 * is visually mapped to the bottom of the glass.  Compositing the 48-pixel
 * control bar here keeps the video at the top and prevents LVGL from ever
 * overwriting it. */
static esp_err_t direct_player_draw_full_lvgl(const uint16_t *decoded,
                                              const uint16_t *controls,
                                              uint16_t *strip)
{
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_OK;
    /* Keep the native QSPI write window advancing from y=0 to y=480.  The
     * controller does not reprogram its row address for every QSPI strip. */
    for (int source_x = LCD_WIDTH - DIRECT_PLAYER_STRIP_WIDTH;
         source_x >= 0 && result == ESP_OK;
         source_x -= DIRECT_PLAYER_STRIP_WIDTH) {
        const int strip_width = (LCD_WIDTH - source_x) < DIRECT_PLAYER_STRIP_WIDTH
            ? (LCD_WIDTH - source_x) : DIRECT_PLAYER_STRIP_WIDTH;
        for (int y = 0; y < LCD_HEIGHT; ++y) {
            for (int x = 0; x < strip_width; ++x) {
                const int logical_x = source_x + x;
                strip[(strip_width - 1 - x) * LCD_HEIGHT + y] =
                    y < VIDEO_CONTENT_HEIGHT ? decoded[y * LCD_WIDTH + logical_x] :
                    controls[logical_x * LCD_HEIGHT + y];
            }
        }
        result = lvgl_port_draw_bitmap_sync(
            s_lvgl_display, 0, LCD_WIDTH - source_x - strip_width,
            LCD_HEIGHT, LCD_WIDTH - source_x, strip, DIRECT_PLAYER_TRANSFER_TIMEOUT_MS);
    }
    lvgl_port_unlock();
    return result;
}

static int direct_player_jpegdec_draw(int source_x, int source_y, int block_width,
                                      int block_height, const uint16_t *pixels, void *user_ctx)
{
    (void)user_ctx;
    if (block_width <= 0 || block_width > LCD_WIDTH || block_height <= 0 ||
        source_x < 0 || source_y < 0 ||
        source_x + block_width > s_direct_player_source_width ||
        source_y + block_height > s_direct_player_source_height) {
        return 0;
    }
    for (int y = 0; y < block_height; ++y) {
        memcpy(&s_direct_player_decoded[(source_y + y) * s_direct_player_source_width + source_x],
               &pixels[y * block_width], block_width * sizeof(uint16_t));
    }
    return 1;
}

static void run_direct_video_player_poc(const char *path)
{
    s_direct_player_done = xSemaphoreCreateBinary();
    if (s_direct_player_done == NULL) {
        ESP_LOGE(TAG, "POC: cannot create QSPI completion semaphore");
        return;
    }
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = direct_player_transfer_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_lcd_io, &callbacks,
                                                               s_direct_player_done));

    uint8_t *frame = heap_caps_malloc(MAX_MJPEG_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *read_buffer = heap_caps_malloc(MJPEG_READ_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_direct_player_decoded = heap_caps_malloc(LCD_WIDTH * VIDEO_CONTENT_HEIGHT * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *strip = heap_caps_malloc(DIRECT_PLAYER_STRIP_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (frame == NULL || read_buffer == NULL || s_direct_player_decoded == NULL || strip == NULL) {
        ESP_LOGE(TAG, "POC: not enough memory (frame=%p reader=%p decoded=%p strip=%p)",
                 frame, read_buffer, s_direct_player_decoded, strip);
        free(frame);
        free(read_buffer);
        free(s_direct_player_decoded);
        free(strip);
        s_direct_player_decoded = NULL;
        return;
    }

    ESP_LOGI(TAG, "POC: direct AXS15231B MJPEG player: %s", path);
#if defined(CONFIG_LAMP_EXPERIMENTAL_VIDEO_HALF_SCALE) && CONFIG_LAMP_EXPERIMENTAL_VIDEO_HALF_SCALE
    const int output_scale = 2;
#else
    const int output_scale = 1;
#endif
    while (true) {
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            ESP_LOGE(TAG, "POC: cannot open %s", path);
            break;
        }
        mjpeg_reader_t reader = { .file = file, .buffer = read_buffer };
        uint32_t frames = 0;
        int64_t decode_us = 0;
        int64_t draw_us = 0;
        const int64_t started_us = esp_timer_get_time();
        size_t frame_size = 0;
        while (read_next_mjpeg_frame(&reader, frame, MAX_MJPEG_FRAME_BYTES, &frame_size)) {
            int width = 0;
            int height = 0;
            s_direct_player_source_width = LCD_WIDTH / output_scale;
            s_direct_player_source_height = VIDEO_CONTENT_HEIGHT / output_scale;
            const int64_t decode_started_us = esp_timer_get_time();
            const esp_err_t decode_result = jpegdec_player_decode(
                frame, frame_size, direct_player_jpegdec_draw, NULL, output_scale, &width, &height);
            decode_us += esp_timer_get_time() - decode_started_us;
            if (decode_result != ESP_OK ||
                (width != LCD_WIDTH / output_scale) ||
                (height != VIDEO_CONTENT_HEIGHT / output_scale)) {
                ESP_LOGE(TAG, "POC: frame must be baseline JPEG 480x272 (got scaled %ux%u; %s)",
                         width, height, esp_err_to_name(decode_result));
                break;
            }
            s_direct_player_source_width = width;
            s_direct_player_source_height = height;
            const int64_t draw_started_us = esp_timer_get_time();
            const esp_err_t draw_result = direct_player_draw_frame(
                s_direct_player_decoded, width, height, strip);
            draw_us += esp_timer_get_time() - draw_started_us;
            if (draw_result != ESP_OK) {
                ESP_LOGE(TAG, "POC: QSPI transfer failed: %s", esp_err_to_name(draw_result));
                break;
            }
            ++frames;
            if ((frames % 30) == 0) {
                const int64_t elapsed_us = esp_timer_get_time() - started_us;
                ESP_LOGI(TAG, "POC: %u frames, %.2f FPS (decode %.1f ms, draw %.1f ms/frame)",
                         frames, frames * 1000000.0 / elapsed_us,
                         decode_us / 1000.0 / frames, draw_us / 1000.0 / frames);
            }
        }
        fclose(file);
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        ESP_LOGI(TAG, "POC complete: %u frames, %.2f FPS", frames,
                 elapsed_us > 0 ? frames * 1000000.0 / elapsed_us : 0.0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    free(frame);
    free(read_buffer);
    free(s_direct_player_decoded);
    free(strip);
    s_direct_player_decoded = NULL;
}

static bool make_companion_audio_path(const char *video_path, char *audio_path, size_t audio_path_size)
{
    strlcpy(audio_path, video_path, audio_path_size);
    char *extension = strrchr(audio_path, '.');
    if (extension == NULL || (size_t)(extension - audio_path) + 5 > audio_path_size) {
        return false;
    }
    /* Preserve WAV support for existing media; prefer it when both formats
     * are present, then accept the MP3 generated by Video Conversion Studio. */
    strcpy(extension, ".wav");
    FILE *audio = fopen(audio_path, "rb");
    if (audio != NULL) {
        fclose(audio);
        return true;
    }
    strcpy(extension, ".mp3");
    audio = fopen(audio_path, "rb");
    if (audio != NULL) {
        fclose(audio);
        return true;
    }
    return false;
}

static void rotate_mjpeg_frame(const uint16_t *decoded, uint16_t *physical, int source_height)
{
    /* Existing media on the SD card is 480x320.  Scale it down just for the
     * 272-pixel video viewport instead of cutting off its bottom 48 lines. */
    if (source_height != VIDEO_CONTENT_HEIGHT) {
        for (int y = 0; y < VIDEO_CONTENT_HEIGHT; ++y) {
            const int source_y = y * source_height / VIDEO_CONTENT_HEIGHT;
            for (int x = 0; x < LCD_WIDTH; ++x) {
                physical[x * LCD_HEIGHT + (LCD_HEIGHT - 1 - y)] =
                    decoded[source_y * LCD_WIDTH + x];
            }
        }
        return;
    }

    /* A tiled transpose keeps both PSRAM reads and writes cache-friendly. */
    uint16_t tile[16 * 16];
    for (int block_y = 0; block_y < VIDEO_CONTENT_HEIGHT; block_y += 16) {
        const int tile_height = (VIDEO_CONTENT_HEIGHT - block_y) < 16 ?
            (VIDEO_CONTENT_HEIGHT - block_y) : 16;
        for (int block_x = 0; block_x < LCD_WIDTH; block_x += 16) {
            const int tile_width = (LCD_WIDTH - block_x) < 16 ? (LCD_WIDTH - block_x) : 16;
            for (int y = 0; y < tile_height; ++y) {
                memcpy(&tile[y * 16], &decoded[(block_y + y) * LCD_WIDTH + block_x],
                       tile_width * sizeof(uint16_t));
            }
            for (int x = 0; x < tile_width; ++x) {
                uint16_t *destination = &physical[(block_x + x) * LCD_HEIGHT +
                                                  (LCD_HEIGHT - 1 - block_y)];
                for (int y = 0; y < tile_height; ++y) {
                    destination[-y] = tile[y * 16 + x];
                }
            }
        }
    }
}

static void video_control_set_pixel(uint16_t *physical, int logical_x, int logical_y, uint16_t color)
{
    if (logical_x < 0 || logical_x >= LCD_WIDTH || logical_y < VIDEO_CONTENT_HEIGHT || logical_y >= LCD_HEIGHT) {
        return;
    }
    /* The direct MJPEG compositor performs the panel rotation.  Keep this
     * small control surface in logical, column-major coordinates. */
    physical[logical_x * LCD_HEIGHT + logical_y] =
        (uint16_t)((color << 8) | (color >> 8));
}

/* The video and slideshow frames bypass LVGL for reliable full-screen QSPI
 * transfers.  Reuse LVGL's Montserrat glyph data here so the persistent
 * control bar nevertheless has the same anti-aliased typeface as the home
 * screen. */
static void draw_video_text_lvgl(uint16_t *physical, const char *text, int logical_y)
{
    const lv_font_t *font = &lv_font_montserrat_20;
    int text_width = 0;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        lv_font_glyph_dsc_t glyph;
        const uint32_t next = (uint8_t)text[index + 1];
        if (lv_font_get_glyph_dsc(font, &glyph, (uint8_t)text[index], next)) {
            text_width += glyph.adv_w;
        }
    }

    int cursor_x = (LCD_WIDTH - text_width) / 2;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        const uint32_t character = (uint8_t)text[index];
        const uint32_t next = (uint8_t)text[index + 1];
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, character, next)) {
            continue;
        }
        const uint8_t advance = glyph.adv_w;
        const uint8_t *bitmap = lv_font_get_glyph_bitmap(glyph.resolved_font, character);
        const int glyph_x = cursor_x + glyph.ofs_x;
        const int glyph_y = logical_y + (font->line_height - font->base_line) -
                            glyph.box_h - glyph.ofs_y;
        const uint32_t max_alpha = (1U << glyph.bpp) - 1U;
        if (bitmap != NULL && glyph.box_w > 0 && glyph.box_h > 0 && max_alpha > 0) {
            for (int row = 0; row < glyph.box_h; ++row) {
                for (int column = 0; column < glyph.box_w; ++column) {
                    const uint32_t bit_offset = ((uint32_t)row * glyph.box_w + column) * glyph.bpp;
                    const uint8_t shift = 8 - glyph.bpp - (bit_offset & 7U);
                    const uint32_t coverage = (bitmap[bit_offset >> 3] >> shift) & max_alpha;
                    if (coverage == 0) {
                        continue;
                    }
                    /* Black text blended over the white control bar. */
                    const uint8_t shade = (uint8_t)(255U - (coverage * 255U) / max_alpha);
                    const uint16_t color = ((uint16_t)(shade >> 3) << 11) |
                                           ((uint16_t)(shade >> 2) << 5) |
                                           (uint16_t)(shade >> 3);
                    video_control_set_pixel(physical, glyph_x + column, glyph_y + row, color);
                }
            }
        }
        cursor_x += advance;
    }
}

static void draw_video_controls(uint16_t *physical, int progress_width)
{
    if (progress_width < 0) {
        progress_width = 0;
    } else if (progress_width > LCD_WIDTH) {
        progress_width = LCD_WIDTH;
    }
    for (int logical_y = VIDEO_CONTENT_HEIGHT; logical_y < LCD_HEIGHT; ++logical_y) {
        for (int logical_x = 0; logical_x < LCD_WIDTH; ++logical_x) {
            video_control_set_pixel(physical, logical_x, logical_y, 0xFFFF);
        }
    }
    /* The thin blue line mirrors the slideshow progress indicator. */
    for (int x = 0; x < progress_width; ++x) {
        for (int y = VIDEO_CONTENT_HEIGHT; y < VIDEO_CONTENT_HEIGHT + 4; ++y) {
            video_control_set_pixel(physical, x, y, 0x0066CC);
        }
    }
    char label[32];
    snprintf(label, sizeof(label), "-   VOLUME %d%%   +", s_audio_volume);
    draw_video_text_lvgl(physical, label, VIDEO_CONTENT_HEIGHT + 12);
}

/* esp_lcd_panel_draw_bitmap() queues QSPI DMA transfers.  The MJPEG buffers
 * live in PSRAM and are reused for the next frame, so explicitly drain that
 * queue before changing or freeing them.  Besides preventing torn frames,
 * this avoids the SPI-DMA underflow seen when a touch stopped playback. */
static bool wait_for_lcd_transfers(void)
{
    const esp_err_t result = esp_lcd_panel_io_tx_param(s_lcd_io, -1, NULL, 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "QSPI transfer drain failed: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}

static bool show_resolve_path(const char *manifest_path, const char *value,
                              char *resolved, size_t resolved_size)
{
    if (value[0] == '/') {
        strlcpy(resolved, value, resolved_size);
        return true;
    }
    const char *slash = strrchr(manifest_path, '/');
    if (slash == NULL) {
        return false;
    }
    const size_t directory_length = (size_t)(slash - manifest_path + 1);
    const int written = snprintf(resolved, resolved_size, "%.*s%s",
                                 (int)directory_length, manifest_path, value);
    return written > 0 && (size_t)written < resolved_size;
}

static bool load_slideshow(const char *manifest_path, slideshow_t *show)
{
    memset(show, 0, sizeof(*show));
    FILE *file = fopen(manifest_path, "r");
    if (file == NULL) {
        return false;
    }
    char line[MAX_SHOW_LINE];
    while (fgets(line, sizeof(line), file) != NULL) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *kind = strtok(line, ";");
        char *first = strtok(NULL, ";");
        char *second = strtok(NULL, "");
        if (kind == NULL || first == NULL) {
            continue;
        }
        trim(kind);
        trim(first);
        if (strcasecmp(kind, "audio") == 0) {
            if (!show_resolve_path(manifest_path, first, show->audio_path,
                                   sizeof(show->audio_path))) {
                fclose(file);
                return false;
            }
        } else if (strcasecmp(kind, "slide") == 0 && second != NULL &&
                   show->slide_count < MAX_SHOW_SLIDES) {
            trim(second);
            char *end = NULL;
            const unsigned long start = strtoul(first, &end, 10);
            if (end == first || *end != '\0' || start > UINT32_MAX ||
                !show_resolve_path(manifest_path, second,
                                   show->slides[show->slide_count].image_path,
                                   sizeof(show->slides[show->slide_count].image_path))) {
                fclose(file);
                return false;
            }
            if (show->slide_count > 0 && start <= show->slides[show->slide_count - 1].start_ms) {
                ESP_LOGE(TAG, "Slideshow timestamps must be ascending: %s", manifest_path);
                fclose(file);
                return false;
            }
            show->slides[show->slide_count++].start_ms = (uint32_t)start;
        }
    }
    fclose(file);
    return show->audio_path[0] != '\0' && show->slide_count > 0 &&
           show->slides[0].start_ms == 0;
}

/* Slides are intentionally rendered through LVGL, rather than sending a
 * competing raw QSPI frame.  The display port owns the panel rotation and
 * transport strips; bypassing it works for continuously refreshed MJPEG but
 * leaves a static show slide partly overwritten by LVGL's own last flush. */
static bool decode_slideshow_slide(const char *path, uint16_t *decoded, lv_img_dsc_t *image)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    const long file_size = ftell(file);
    rewind(file);
    if (file_size <= 0 || file_size > MAX_INFO_IMAGE_BYTES) {
        fclose(file);
        return false;
    }
    uint8_t *encoded = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (encoded == NULL || fread(encoded, 1, file_size, file) != (size_t)file_size) {
        free(encoded);
        fclose(file);
        return false;
    }
    fclose(file);
    esp_jpeg_image_cfg_t config = {
        .indata = encoded,
        .indata_size = file_size,
        .outbuf = (uint8_t *)decoded,
        .outbuf_size = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 1 },
    };
    esp_jpeg_image_output_t output = {0};
    const esp_err_t result = esp_jpeg_decode(&config, &output);
    free(encoded);
    if (result != ESP_OK || output.width != LCD_WIDTH ||
        output.height != VIDEO_CONTENT_HEIGHT) {
        ESP_LOGE(TAG, "Slide needs baseline JPEG 480x272 (got %ux%u): %s",
                 output.width, output.height, path);
        return false;
    }
    memset(image, 0, sizeof(*image));
    image->header.cf = LV_IMG_CF_TRUE_COLOR;
    image->header.w = output.width;
    image->header.h = output.height;
    image->data_size = output.output_len;
    image->data = (const uint8_t *)decoded;
    return true;
}

static void play_slideshow(const media_entry_t *entry)
{
    /* A 64-slide manifest is about 8.5 KiB.  audio_task has a 6 KiB stack,
     * so keep it in PSRAM; placing it on the stack corrupts FreeRTOS task
     * lists as soon as a show is started. */
    slideshow_t *show = heap_caps_calloc(1, sizeof(*show), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (show == NULL) {
        screen_message(0xB000, "SHOW ERROR", "NOT ENOUGH MEMORY");
        return;
    }
    if (!load_slideshow(entry->path, show)) {
        free(show);
        screen_message(0xB000, "SHOW ERROR", "CHECK show.csv");
        vTaskDelay(pdMS_TO_TICKS(1200));
        screen_message(0xD680, "SCAN EEN QR", "GEREED");
        return;
    }
    uint16_t *decoded = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoded == NULL) {
        free(decoded);
        free(show);
        screen_message(0xB000, "SHOW ERROR", "NOT ENOUGH MEMORY");
        return;
    }

    if (!lvgl_port_lock(portMAX_DELAY)) {
        free(decoded);
        free(show);
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *slide = lv_img_create(screen);
    lv_obj_align(slide, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *controls = lv_obj_create(screen);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, LCD_WIDTH, VIDEO_CONTROL_HEIGHT);
    lv_obj_set_pos(controls, 0, VIDEO_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(controls, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(controls, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *progress = lv_obj_create(controls);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, 0, 4);
    lv_obj_set_pos(progress, 0, 0);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x0066CC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
    s_volume_label = lv_label_create(controls);
    lv_obj_set_width(s_volume_label, LCD_WIDTH);
    lv_obj_set_style_text_color(s_volume_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_volume_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_volume_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text_fmt(s_volume_label, "-   VOLUME %d%%   +", s_audio_volume);
    lv_obj_center(s_volume_label);
    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    lvgl_port_unlock();

    video_audio_context_t audio = {0};
    strlcpy(audio.path, show->audio_path, sizeof(audio.path));
    audio.owner = xTaskGetCurrentTaskHandle();
    const BaseType_t task_created = xTaskCreate(slideshow_audio_task, "show_audio", 6144,
                                                &audio, 6, NULL);
    if (task_created != pdPASS || ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500)) == 0 ||
        !audio.started) {
        audio.stop_requested = true;
        while (task_created == pdPASS && !audio.finished) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
        free(decoded);
        free(show);
        screen_message(0xB000, "SHOW ERROR", "AUDIO FILE");
        return;
    }

    const int64_t start_us = esp_timer_get_time();
    const uint32_t duration_ms = path_has_extension(show->audio_path, ".wav")
        ? wav_duration_ms(show->audio_path) : 0;
    uint32_t last_progress_draw_ms = UINT32_MAX;
    size_t next_slide = 0;
    bool failed = false;
    s_volume_redraw_pending = false;
    lv_img_dsc_t slide_image = {0};
    while (!audio.finished) {
        if (stop_media_on_touch(NULL)) {
            ESP_LOGI(TAG, "Slideshow stopped by touch");
            break;
        }
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        size_t due_slide = next_slide;
        while (due_slide < show->slide_count && show->slides[due_slide].start_ms <= elapsed_ms) {
            ++due_slide;
        }
        if (duration_ms != 0 && (last_progress_draw_ms == UINT32_MAX ||
                                 elapsed_ms - last_progress_draw_ms >= 250)) {
            const int width = (int)(((uint64_t)elapsed_ms * LCD_WIDTH) / duration_ms);
            if (lvgl_port_lock(portMAX_DELAY)) {
                lv_obj_set_width(progress, width > LCD_WIDTH ? LCD_WIDTH : width);
                lv_obj_invalidate(progress);
                lv_refr_now(s_lvgl_display);
                lvgl_port_unlock();
                last_progress_draw_ms = elapsed_ms;
            }
        }
        if (due_slide != next_slide) {
            next_slide = due_slide;
            if (!decode_slideshow_slide(show->slides[next_slide - 1].image_path, decoded, &slide_image)) {
                failed = true;
                break;
            }
            if (!lvgl_port_lock(portMAX_DELAY)) {
                failed = true;
                break;
            }
            lv_img_set_src(slide, &slide_image);
            lv_obj_invalidate(slide);
            lv_refr_now(s_lvgl_display);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    audio.stop_requested = true;
    while (!audio.finished) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    s_volume_label = NULL;
    free(decoded);
    free(show);
    if (failed) {
        screen_message(0xB000, "SHOW ERROR", "SLIDE FILE");
        vTaskDelay(pdMS_TO_TICKS(1200));
    } else {
        vTaskDelay(pdMS_TO_TICKS(POST_PLAYBACK_HOME_DELAY_MS));
    }
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
}

static void play_mjpeg(const media_entry_t *entry)
{
    FILE *file = fopen(entry->path, "rb");
    if (file == NULL) {
        screen_message(0xB000, "FILE ERROR", entry->title);
        vTaskDelay(pdMS_TO_TICKS(FILE_ERROR_HOME_DELAY_MS));
        screen_message(0xD680, "SCAN EEN QR", "GEREED");
        return;
    }
    uint8_t *frame = heap_caps_malloc(MAX_MJPEG_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *read_buffer = heap_caps_malloc(MJPEG_READ_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *decoded = heap_caps_aligned_calloc(16, LCD_WIDTH * VIDEO_CONTENT_HEIGHT,
                                                 sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *controls = heap_caps_calloc(LCD_WIDTH * LCD_HEIGHT, sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *strip = heap_caps_malloc(DIRECT_PLAYER_STRIP_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (frame == NULL || read_buffer == NULL || decoded == NULL || controls == NULL || strip == NULL) {
        ESP_LOGE(TAG, "Not enough PSRAM for MJPEG");
        free(frame);
        free(read_buffer);
        free(decoded);
        free(controls);
        free(strip);
        fclose(file);
        screen_message(0xB000, "VIDEO ERROR", "NOT ENOUGH MEMORY");
        return;
    }
    size_t total_bytes = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long file_size = ftell(file);
        if (file_size > 0) {
            total_bytes = (size_t)file_size;
        }
        rewind(file);
    }

    /* The static LVGL scene is just a black hand-off surface.  Every video
     * frame, including the volume controls, is directly composed below. */
    if (!lvgl_port_lock(portMAX_DELAY)) {
        fclose(file);
        free(frame);
        free(read_buffer);
        free(decoded);
        free(controls);
        free(strip);
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    const esp_err_t idle_result = lvgl_port_wait_display_idle(s_lvgl_display,
                                                               DIRECT_PLAYER_TRANSFER_TIMEOUT_MS);
    lvgl_port_unlock();
    if (idle_result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot synchronise MJPEG display: %s", esp_err_to_name(idle_result));
        s_volume_label = NULL;
        fclose(file);
        free(frame);
        free(read_buffer);
        free(decoded);
        free(controls);
        free(strip);
        return;
    }

    mjpeg_reader_t reader = {
        .file = file,
        .buffer = read_buffer,
    };
#if CONFIG_LAMP_MJPEG_USE_NEW_JPEG
    new_jpeg_player_t *new_jpeg = NULL;
    const esp_err_t new_jpeg_open_result = new_jpeg_player_open(&new_jpeg);
    if (new_jpeg_open_result != ESP_OK) {
        ESP_LOGW(TAG, "esp_new_jpeg unavailable, using JPEGDEC: %s",
                 esp_err_to_name(new_jpeg_open_result));
    }
#endif
    video_audio_context_t video_audio = {0};
    bool has_video_audio = make_companion_audio_path(entry->path, video_audio.path, sizeof(video_audio.path));
    if (has_video_audio) {
        video_audio.owner = xTaskGetCurrentTaskHandle();
        const BaseType_t task_created = xTaskCreate(video_audio_task, "video_audio", 6144,
                                                    &video_audio, 6, NULL);
        if (task_created == pdPASS) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500));
        }
        if (task_created != pdPASS || !video_audio.started) {
            ESP_LOGW(TAG, "No usable companion audio: %s", video_audio.path);
            video_audio.stop_requested = true;
            while (task_created == pdPASS && !video_audio.finished) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            }
            has_video_audio = false;
        }
    }
    const uint8_t video_fps = entry->video_fps > 0 ? entry->video_fps : CONFIG_LAMP_VIDEO_FPS;
    const int64_t frame_interval_us = 1000000LL / video_fps;
    ESP_LOGI(TAG, "MJPEG FPS: %u%s", (unsigned)video_fps,
             entry->video_fps > 0 ? " from media-map" : " from firmware default");
    const int64_t playback_start_us = esp_timer_get_time();
    uint32_t frame_index = 0;
    uint32_t dropped_frames = 0;
    uint32_t displayed_frames = 0;
    uint32_t new_jpeg_frames = 0;
    uint32_t fallback_jpegdec_frames = 0;
    int64_t decode_us = 0;
    int64_t draw_us = 0;
    size_t frame_size = 0;
    int last_progress_width = -1;
    s_volume_redraw_pending = false;
    draw_video_controls(controls, 0);
    s_direct_player_decoded = decoded;
    s_direct_player_source_width = LCD_WIDTH;
    s_direct_player_source_height = VIDEO_CONTENT_HEIGHT;
    while (read_next_mjpeg_frame(&reader, frame, MAX_MJPEG_FRAME_BYTES, &frame_size)) {
        if (stop_media_on_touch(NULL)) {
            ESP_LOGI(TAG, "Video stopped by touch");
            break;
        }
        while (esp_timer_get_time() > playback_start_us + (frame_index + 1) * frame_interval_us &&
               read_next_mjpeg_frame(&reader, frame, MAX_MJPEG_FRAME_BYTES, &frame_size)) {
            ++frame_index;
            ++dropped_frames;
        }
        if (has_video_audio && video_audio.finished) {
            break;
        }
        int width = 0;
        int height = 0;
        esp_err_t decode_result = ESP_FAIL;
        const int64_t decode_started_us = esp_timer_get_time();
#if CONFIG_LAMP_MJPEG_USE_NEW_JPEG
        if (new_jpeg != NULL) {
            decode_result = new_jpeg_player_decode(new_jpeg, frame, frame_size,
                                                   decoded,
                                                   LCD_WIDTH * VIDEO_CONTENT_HEIGHT,
                                                   &width, &height);
            if (decode_result == ESP_OK) {
                ++new_jpeg_frames;
            }
        }
#endif
        if (decode_result != ESP_OK) {
            decode_result = jpegdec_player_decode(frame, frame_size, direct_player_jpegdec_draw,
                                                  NULL, 1, &width, &height);
            if (decode_result == ESP_OK) {
                ++fallback_jpegdec_frames;
            }
        }
        decode_us += esp_timer_get_time() - decode_started_us;
        if (decode_result != ESP_OK || width != LCD_WIDTH || height != VIDEO_CONTENT_HEIGHT) {
            ESP_LOGE(TAG, "MJPEG needs baseline JPEG 480x272 (got %ux%u)", width, height);
            break;
        }
        /* The reader buffers ahead, so subtract unread bytes to obtain the
         * true stream position.  This avoids a second, potentially blocking
         * SD-card scan solely for the progress bar. */
        const long buffered_end = ftell(file);
        const size_t unread_bytes = reader.length - reader.position;
        const size_t consumed_bytes = buffered_end > 0 && (size_t)buffered_end > unread_bytes
            ? (size_t)buffered_end - unread_bytes : 0;
        const int progress_width = total_bytes == 0 ? 0 : (int)(((uint64_t)consumed_bytes *
            LCD_WIDTH) / total_bytes);
        if (s_volume_redraw_pending || progress_width != last_progress_width) {
            memset(controls, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
            draw_video_controls(controls, progress_width);
            s_volume_redraw_pending = false;
            last_progress_width = progress_width;
        }
        const int64_t draw_started_us = esp_timer_get_time();
        const esp_err_t draw_result = direct_player_draw_full_lvgl(decoded, controls, strip);
        draw_us += esp_timer_get_time() - draw_started_us;
        if (draw_result != ESP_OK) {
            ESP_LOGE(TAG, "MJPEG QSPI transfer failed: %s", esp_err_to_name(draw_result));
            break;
        }
        ++displayed_frames;
        ++frame_index;
        const int64_t next_frame_us = playback_start_us + frame_index * frame_interval_us;
        const int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        }
    }
    if (has_video_audio) {
        video_audio.stop_requested = true;
        while (!video_audio.finished) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "MJPEG: %u displayed, %u dropped, %u esp_new_jpeg, %u JPEGDEC, %.1f ms decode, %.1f ms draw",
             displayed_frames, dropped_frames, new_jpeg_frames, fallback_jpegdec_frames,
             displayed_frames == 0 ? 0.0 : decode_us / 1000.0 / displayed_frames,
             displayed_frames == 0 ? 0.0 : draw_us / 1000.0 / displayed_frames);
#if CONFIG_LAMP_MJPEG_USE_NEW_JPEG
    new_jpeg_player_close(new_jpeg);
#endif
    s_volume_label = NULL;
    s_direct_player_decoded = NULL;
    fclose(file);
    free(frame);
    free(read_buffer);
    free(decoded);
    free(controls);
    free(strip);
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
}

static void play_avi(const media_entry_t *entry)
{
    FILE *file = fopen(entry->path, "rb");
    if (file == NULL) {
        screen_message(0xB000, "FILE ERROR", entry->title);
        vTaskDelay(pdMS_TO_TICKS(FILE_ERROR_HOME_DELAY_MS));
        screen_message(0xD680, "SCAN EEN QR", "GEREED");
        return;
    }

    avi_reader_t reader = {0};
    if (!avi_reader_open(file, &reader) ||
        reader.width != LCD_WIDTH || reader.height != VIDEO_CONTENT_HEIGHT ||
        strcasecmp(reader.compressor, "cvid") != 0) {
        ESP_LOGE(TAG, "AVI needs Cinepak/cvid 480x272 (got %ux%u %.4s)",
                 (unsigned)reader.width, (unsigned)reader.height, reader.compressor);
        fclose(file);
        screen_message(0xB000, "AVI ERROR", "USE CVID 480x272");
        vTaskDelay(pdMS_TO_TICKS(FILE_ERROR_HOME_DELAY_MS));
        screen_message(0xD680, "SCAN EEN QR", "GEREED");
        return;
    }

    uint8_t *packet = heap_caps_malloc(MAX_MJPEG_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *decoded = heap_caps_calloc(LCD_WIDTH * VIDEO_CONTENT_HEIGHT, sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *controls = heap_caps_calloc(LCD_WIDTH * LCD_HEIGHT, sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *strip = heap_caps_malloc(DIRECT_PLAYER_STRIP_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    cinepak_decoder_t *decoder = heap_caps_malloc(sizeof(cinepak_decoder_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet == NULL || decoded == NULL || controls == NULL ||
        strip == NULL || decoder == NULL) {
        ESP_LOGE(TAG, "Not enough PSRAM for AVI/Cinepak");
        free(packet);
        free(decoded);
        free(controls);
        free(strip);
        free(decoder);
        fclose(file);
        screen_message(0xB000, "AVI ERROR", "NOT ENOUGH MEMORY");
        return;
    }
    cinepak_decoder_init(decoder, LCD_WIDTH, VIDEO_CONTENT_HEIGHT);

    if (!lvgl_port_lock(portMAX_DELAY)) {
        fclose(file);
        free(packet);
        free(decoded);
        free(controls);
        free(strip);
        free(decoder);
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    s_volume_label = NULL;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(screen);
    lv_refr_now(s_lvgl_display);
    const esp_err_t idle_result = lvgl_port_wait_display_idle(s_lvgl_display,
                                                               DIRECT_PLAYER_TRANSFER_TIMEOUT_MS);
    lvgl_port_unlock();
    if (idle_result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot synchronise AVI display: %s", esp_err_to_name(idle_result));
        s_volume_label = NULL;
        fclose(file);
        free(packet);
        free(decoded);
        free(controls);
        free(strip);
        free(decoder);
        return;
    }

    video_audio_context_t video_audio = {0};
    bool has_video_audio = make_companion_audio_path(entry->path, video_audio.path,
                                                     sizeof(video_audio.path));
    if (has_video_audio) {
        video_audio.owner = xTaskGetCurrentTaskHandle();
        const BaseType_t task_created = xTaskCreate(video_audio_task, "video_audio", 6144,
                                                    &video_audio, 6, NULL);
        if (task_created == pdPASS) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500));
        }
        if (task_created != pdPASS || !video_audio.started) {
            ESP_LOGW(TAG, "No usable companion audio: %s", video_audio.path);
            video_audio.stop_requested = true;
            while (task_created == pdPASS && !video_audio.finished) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            }
            has_video_audio = false;
        }
    }

    const int64_t frame_interval_us = reader.frame_interval_us > 0
        ? reader.frame_interval_us : 1000000LL / CONFIG_LAMP_VIDEO_FPS;
    const int64_t playback_start_us = esp_timer_get_time();
    uint32_t frame_index = 0;
    uint32_t displayed_frames = 0;
    int64_t decode_us = 0;
    int64_t draw_us = 0;
    int last_progress_width = -1;
    size_t packet_size = 0;
    s_volume_redraw_pending = false;
    draw_video_controls(controls, 0);

    while (avi_reader_next_video_packet(&reader, packet, MAX_MJPEG_FRAME_BYTES, &packet_size)) {
        if (stop_media_on_touch(NULL)) {
            ESP_LOGI(TAG, "AVI video stopped by touch");
            break;
        }
        if (has_video_audio && video_audio.finished) {
            break;
        }
        const int64_t decode_started_us = esp_timer_get_time();
        if (!cinepak_decoder_decode(decoder, packet, packet_size, decoded)) {
            ESP_LOGE(TAG, "Cinepak frame decode failed");
            screen_message(0xB000, "AVI ERROR", "BAD FRAME");
            vTaskDelay(pdMS_TO_TICKS(FILE_ERROR_HOME_DELAY_MS));
            break;
        }
        decode_us += esp_timer_get_time() - decode_started_us;

        const int progress_width = avi_reader_progress_width(&reader, LCD_WIDTH);
        if (s_volume_redraw_pending || progress_width != last_progress_width) {
            memset(controls, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
            draw_video_controls(controls, progress_width);
            s_volume_redraw_pending = false;
            last_progress_width = progress_width;
        }

        const int64_t draw_started_us = esp_timer_get_time();
        const esp_err_t draw_result = direct_player_draw_full_lvgl(decoded, controls, strip);
        draw_us += esp_timer_get_time() - draw_started_us;
        if (draw_result != ESP_OK) {
            ESP_LOGE(TAG, "AVI QSPI transfer failed: %s", esp_err_to_name(draw_result));
            break;
        }
        ++displayed_frames;
        ++frame_index;

        const int64_t next_frame_us = playback_start_us + frame_index * frame_interval_us;
        const int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        }
    }

    if (has_video_audio) {
        video_audio.stop_requested = true;
        while (!video_audio.finished) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "AVI/Cinepak: %u frames, %.1f ms decode, %.1f ms draw",
             displayed_frames,
             displayed_frames == 0 ? 0.0 : decode_us / 1000.0 / displayed_frames,
             displayed_frames == 0 ? 0.0 : draw_us / 1000.0 / displayed_frames);
    s_volume_label = NULL;
    fclose(file);
    free(packet);
    free(decoded);
    free(controls);
    free(strip);
    free(decoder);
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
}

static void audio_task(void *argument)
{
    media_entry_t entry;
    while (true) {
        if (xQueueReceive(s_play_queue, &entry, portMAX_DELAY) == pdTRUE) {
            if (path_has_extension(entry.path, ".wav")) {
                play_wav(&entry);
            } else if (path_has_extension(entry.path, ".mp3")) {
                screen_message(0x05A0, "PLAYING MP3", "TAP SCREEN TO STOP");
                const esp_err_t result = mp3_play_file(entry.path, &s_audio_volume,
                                                       stop_media_on_touch, NULL);
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "MP3 playback failed: %s", esp_err_to_name(result));
                    screen_message(0xB000, "MP3 ERROR", entry.title);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(POST_PLAYBACK_HOME_DELAY_MS));
                }
                screen_message(0xD680, "SCAN EEN QR", "GEREED");
            } else if (path_has_extension(entry.path, ".mjpeg") || path_has_extension(entry.path, ".mjpg")) {
                play_mjpeg(&entry);
            } else if (path_has_extension(entry.path, ".avi")) {
                play_avi(&entry);
            } else if (path_has_extension(entry.path, ".csv")) {
                play_slideshow(&entry);
            } else if (path_has_extension(entry.path, ".jpg") || path_has_extension(entry.path, ".jpeg")) {
                play_info_page(&entry);
            } else {
                ESP_LOGW(TAG, "Unsupported media file: %s", entry.path);
                screen_message(0xB000, "UNSUPPORTED FILE", entry.title);
            }
            s_media_playing = false;
            /* A GM861 can report the same code twice while it remains in
             * view.  Do not immediately restart the just-stopped media. */
            s_qr_ignore_until_us = esp_timer_get_time() + QR_RETRIGGER_GUARD_US;
        }
    }
}

static void touch_task(void *argument)
{
    bool was_pressed = false;
    int64_t first_corner_tap_us = 0;
    while (true) {
        if (s_usb_error_reset_pending) {
            uint16_t x = 0;
            uint16_t y = 0;
            const bool pressed = touch_get_logical_point(&x, &y);
            if (pressed && !was_pressed) {
                ESP_LOGW(TAG, "Touch reset requested after USB error");
                esp_restart();
            }
            was_pressed = pressed;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (s_maintenance_menu_active) {
            uint16_t x = 0;
            uint16_t y = 0;
            const bool pressed = touch_get_logical_point(&x, &y);
            if (pressed && !was_pressed) {
                s_maintenance_menu_active = false;
                if (y < LCD_HEIGHT / 2) {
                    ESP_LOGI(TAG, "USB maintenance mode selected");
                    start_usb_msc_mode();
                } else {
                    ESP_LOGI(TAG, "Module reset selected");
                    esp_restart();
                }
            }
            was_pressed = pressed;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (!s_media_playing && !s_usb_msc_active) {
            uint16_t x = 0;
            uint16_t y = 0;
            const bool pressed = touch_get_logical_point(&x, &y);
            if (pressed && !was_pressed) {
                const int64_t now = esp_timer_get_time();
                const bool top_left = x < MAINTENANCE_CORNER_SIZE && y < MAINTENANCE_CORNER_SIZE;
                const bool bottom_right = x >= LCD_WIDTH - MAINTENANCE_CORNER_SIZE &&
                                          y >= LCD_HEIGHT - MAINTENANCE_CORNER_SIZE;
                if (top_left) {
                    first_corner_tap_us = now;
                    ESP_LOGI(TAG, "Maintenance gesture: first corner accepted");
                } else if (bottom_right && first_corner_tap_us != 0 &&
                           now - first_corner_tap_us <= MAINTENANCE_GESTURE_TIMEOUT_US) {
                    first_corner_tap_us = 0;
                    s_maintenance_menu_active = true;
                    ESP_LOGI(TAG, "Maintenance gesture: opening menu");
                    show_maintenance_menu();
                } else if (y >= LCD_HEIGHT - 70) {
                    first_corner_tap_us = 0;
                    adjust_audio_volume(x < LCD_WIDTH / 2 ? -10 : 10);
                } else {
                    first_corner_tap_us = 0;
                }
            }
            if (first_corner_tap_us != 0 &&
                esp_timer_get_time() - first_corner_tap_us > MAINTENANCE_GESTURE_TIMEOUT_US) {
                first_corner_tap_us = 0;
            }
            was_pressed = pressed;
        } else {
            was_pressed = false;
            first_corner_tap_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void handle_qr_code(char *code)
{
    trim(code);
    if (code[0] == '\0') {
        return;
    }

    if (s_usb_msc_active || s_media_playing || esp_timer_get_time() < s_qr_ignore_until_us) {
        ESP_LOGI(TAG, "QR ignored while media is active/cooling down: %s", code);
        return;
    }

    ESP_LOGI(TAG, "QR: %s", code);
    const media_entry_t *entry = find_media(code);
    if (entry == NULL) {
        screen_message(0xB000, "UNKNOWN QR", code);
        /* Keep the explanation visible, then reliably restore the ready
         * screen.  Ignore repeated scanner reports while the message is up. */
        s_qr_ignore_until_us = esp_timer_get_time() + UNKNOWN_QR_MESSAGE_MS * 1000LL;
        vTaskDelay(pdMS_TO_TICKS(UNKNOWN_QR_MESSAGE_MS));
        if (!s_usb_msc_active && !s_media_playing) {
            screen_message(0xD680, "SCAN EEN QR", "GEREED");
        }
    } else {
        /* Reserve playback before sending so duplicate UART reports cannot
         * add a second copy while the audio task is about to wake up. */
        s_media_playing = true;
        if (xQueueSend(s_play_queue, entry, 0) != pdTRUE) {
            s_media_playing = false;
            screen_message(0xB000, "PLEASE WAIT", "AUDIO QUEUE FULL");
        }
    }
}

static void qr_probe_scanner(void)
{
    // GM861 "find baud rate" command. At its factory-default 9600 8N1 setting
    // the scanner answers with a short binary acknowledgement.
    static const uint8_t find_baud_rate[] = {
        0x7E, 0x00, 0x07, 0x01, 0x00, 0x2A, 0x02, 0xD8, 0x0F,
    };
    // Read GM861 zone bit 0x000D.  Its low two bits select serial, USB
    // keyboard, or USB virtual-serial output.  AB CD disables CRC checking.
    static const uint8_t read_output_mode[] = {
        0x7E, 0x00, 0x07, 0x01, 0x00, 0x0D, 0x01, 0xAB, 0xCD,
    };
    // Preserve the other 0x000D settings, but replace USB-keyboard output
    // (0xA1) by TTL serial output (0xA0).  This is deliberately not saved
    // to the scanner's flash until it has been verified with a real QR scan.
    static const uint8_t set_output_serial[] = {
        0x7E, 0x00, 0x08, 0x01, 0x00, 0x0D, 0xA0, 0xAB, 0xCD,
    };
    uint8_t response[32];

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_ERROR_CHECK(uart_flush_input(QR_UART));
    const int sent = uart_write_bytes(QR_UART, find_baud_rate, sizeof(find_baud_rate));
    ESP_LOGI(TAG, "Querying GM861 serial settings on GPIO%d/GPIO%d (%d bytes)",
             CONFIG_LAMP_QR_TX_GPIO, CONFIG_LAMP_QR_RX_GPIO, sent);
    ESP_ERROR_CHECK(uart_wait_tx_done(QR_UART, pdMS_TO_TICKS(100)));

    int received = uart_read_bytes(QR_UART, response, sizeof(response), pdMS_TO_TICKS(400));
    if (received > 0) {
        ESP_LOGI(TAG, "GM861 replied with %d bytes", received);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, response, received, ESP_LOG_INFO);
    } else {
        ESP_LOGW(TAG, "No GM861 reply; check scanner RXD -> GPIO18 or its serial mode");
    }

    ESP_ERROR_CHECK(uart_flush_input(QR_UART));
    const int mode_query_sent = uart_write_bytes(QR_UART, read_output_mode, sizeof(read_output_mode));
    if (mode_query_sent != sizeof(read_output_mode)) {
        ESP_LOGW(TAG, "Could not send complete GM861 output-mode query (%d bytes)", mode_query_sent);
        return;
    }
    ESP_ERROR_CHECK(uart_wait_tx_done(QR_UART, pdMS_TO_TICKS(100)));
    received = uart_read_bytes(QR_UART, response, sizeof(response), pdMS_TO_TICKS(400));
    if (received >= 5) {
        static const char *const mode_names[] = {
            "TTL serial", "USB keyboard", "reserved", "USB virtual serial",
        };
        const uint8_t mode = response[4] & 0x03;
        ESP_LOGI(TAG, "GM861 output mode: %s (zone 0x000D = 0x%02X)",
                 mode_names[mode], response[4]);
        if (mode == 1) {
            ESP_ERROR_CHECK(uart_flush_input(QR_UART));
            const int set_sent = uart_write_bytes(QR_UART, set_output_serial, sizeof(set_output_serial));
            if (set_sent == sizeof(set_output_serial)) {
                ESP_ERROR_CHECK(uart_wait_tx_done(QR_UART, pdMS_TO_TICKS(100)));
                received = uart_read_bytes(QR_UART, response, sizeof(response), pdMS_TO_TICKS(400));
                if (received >= 5 && response[4] == 0x00) {
                    ESP_LOGI(TAG, "GM861 temporarily switched to TTL serial output");
                } else {
                    ESP_LOGW(TAG, "GM861 did not acknowledge the serial-output change");
                }
            } else {
                ESP_LOGW(TAG, "Could not send GM861 serial-output change (%d bytes)", set_sent);
            }
        }
    } else {
        ESP_LOGW(TAG, "Could not read GM861 output-mode setting");
        if (received > 0) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, response, received, ESP_LOG_WARN);
        }
    }
}

static void qr_task(void *argument)
{
    char code[MAX_QR_TEXT] = {0};
    size_t length = 0;

    qr_probe_scanner();
    while (true) {
        uint8_t byte;
        const int received = uart_read_bytes(QR_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (received == 1) {
            ESP_LOGI(TAG, "QR UART byte: 0x%02X", byte);
            if (byte == '\r' || byte == '\n') {
                code[length] = '\0';
                if (length > 0) {
                    handle_qr_code(code);
                }
                length = 0;
            } else if (isprint(byte) && length + 1 < sizeof(code)) {
                code[length++] = (char)byte;
            }
        } else if (length > 0) {
            // The GM861's standard serial output has no CR/LF suffix.  A quiet
            // period after the fast UART burst therefore marks the end of a code.
            code[length] = '\0';
            handle_qr_code(code);
            length = 0;
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
    ESP_ERROR_CHECK(uart_set_pin(QR_UART, CONFIG_LAMP_QR_TX_GPIO, CONFIG_LAMP_QR_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
#if defined(CONFIG_LAMP_EXPERIMENTAL_VIDEO_POC) && CONFIG_LAMP_EXPERIMENTAL_VIDEO_POC
    /* Deliberately do not initialise LVGL in the player POC: it must have
     * exclusive ownership of the panel IO callback and QSPI transport. */
    lcd_panel_init();
    const esp_err_t poc_sd_result = mount_sd_card();
    if (poc_sd_result != ESP_OK) {
        ESP_LOGE(TAG, "POC: SD mount failed: %s", esp_err_to_name(poc_sd_result));
        return;
    }
    run_direct_video_player_poc(CONFIG_LAMP_EXPERIMENTAL_VIDEO_PATH);
    return;
#endif
    s_screen_lock = xSemaphoreCreateMutex();
    s_play_queue = xQueueCreate(2, sizeof(media_entry_t));
    if (s_screen_lock == NULL || s_play_queue == NULL) {
        ESP_LOGE(TAG, "Cannot allocate application resources");
        return;
    }

    lcd_init();
    touch_init();
    screen_message(0xD680, "QR LAMP", "MOUNTING SD");
    const esp_err_t sd_result = mount_sd_card();
    if (sd_result != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(sd_result));
        screen_message(0xB000, "SD ERROR", "CHECK CARD");
        return;
    }
    load_logo();
    load_media_map();
    if (s_media_count == 0) {
        screen_message(0xB000, "MAP ERROR", "CHECK SD CARD");
        return;
    }
    qr_uart_init();
    screen_message(0xD680, "SCAN EEN QR", "GEREED");
    xTaskCreate(audio_task, "audio", 6144, NULL, 5, NULL);
    xTaskCreate(qr_task, "qr_reader", 4096, NULL, 6, NULL);
    xTaskCreate(touch_task, "touch", 3072, NULL, 4, NULL);
}
