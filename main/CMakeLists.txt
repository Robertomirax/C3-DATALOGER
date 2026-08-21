#---------------- CMakeLists.txt --------------------------
idf_component_register(
    SRCS "hardware.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        freertos
        esp_lcd 
        esp_driver_uart 
        esp_driver_gpio 
        esp_driver_spi 
        espressif__esp_lvgl_port
)