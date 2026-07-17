set(BOARD_DRIVER_DIR "${CMAKE_CURRENT_LIST_DIR}/../docs/1-Demo/Demo_Arduino/DEMO_MJPEG")

idf_component_register(
    SRCS "main.c" "${BOARD_DRIVER_DIR}/esp_lcd_axs15231b.c"
    INCLUDE_DIRS "." "${BOARD_DRIVER_DIR}"
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
)
