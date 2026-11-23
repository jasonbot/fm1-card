idf_component_register(
  SRCS
    "src/board.cpp"
    "src/config.cpp"
    "src/file.cpp"
    "src/library.cpp"
    "src/net.cpp"
    "src/usb.cpp"
    "src/flash.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    fm1_core
    lwjson
    tinyusb
    esp_lcd
    esp_timer
    esp_wifi
    esp_http_client
    esp_netif
    esp_event
    fatfs
    sdmmc
    esp_driver_sdspi
    esp_driver_sdmmc
    nvs_flash
    esp_http_server
    esp_app_format
    freertos
  PRIV_REQUIRES
    esp_driver_spi
    esp_hw_support
    esp_driver_i2c
    esp_driver_gpio
    esp_adc
    mbedtls)

# Binary assets (MIT; see data/m-bootloader-LICENSE.txt).
target_add_binary_data(${COMPONENT_LIB} "../../data/mflr-loader-a.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../../data/bootloader-fm1.boot" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../../data/bootloader-smk37.boot" BINARY)
