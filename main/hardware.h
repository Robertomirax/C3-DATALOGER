#ifndef HARDWARE_H
#define HARDWARE_H

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- PINES ESP32-C3 SUPER MINI ---
#define LCD_HOST          SPI2_HOST
#define PIN_NUM_SCLK      4
#define PIN_NUM_MOSI      6
#define PIN_NUM_LCD_DC    2
#define PIN_NUM_LCD_RST   3
#define PIN_NUM_LCD_CS   -1
#define PIN_NUM_BK_LIGHT  1

#define LCD_H_RES         240
#define LCD_V_RES         240

// --- CONFIGURACIÓN UART / MAX3232 ---
#define UART_PORT_NUM     UART_NUM_1
#define UART_TX_PIN       GPIO_NUM_21
#define UART_RX_PIN       GPIO_NUM_20
#define UART_BUF_SIZE     1024
#define BAUD_RATE         300

// --- OTROS PINES ---
#define BLINK_GPIO        GPIO_NUM_8

extern lv_disp_t *lvgl_disp;

void hardware_init_gpio(void);
void hardware_init_uart(void);
lv_disp_t* hardware_init_display(void);
void hardware_init_all(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H