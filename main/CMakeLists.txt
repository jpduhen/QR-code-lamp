set(BOARD_DRIVER_DIR "${CMAKE_CURRENT_LIST_DIR}/../docs/1-Demo/Demo_Arduino/DEMO_MJPEG")
set(JPEGDEC_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/jpegdec/src")

idf_component_register(
    SRCS "main.c" "mp3_player.cpp" "jpegdec_player.cpp" "avi_reader.c" "cinepak_decoder.c" "${JPEGDEC_DIR}/JPEGDEC.cpp" "${JPEGDEC_DIR}/s3_simd_420.S" "${JPEGDEC_DIR}/s3_simd_444.S" "${JPEGDEC_DIR}/s3_simd_dequant.S" "${BOARD_DRIVER_DIR}/esp_lcd_axs15231b.c" "${BOARD_DRIVER_DIR}/esp_lcd_touch.c" "${BOARD_DRIVER_DIR}/lv_port.c"
    INCLUDE_DIRS "." "${BOARD_DRIVER_DIR}" "${JPEGDEC_DIR}"
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_spi
        esp_driver_uart
        esp_lcd
        fatfs
        sdmmc
        vfs
        lvgl
        esp_tinyusb
)
