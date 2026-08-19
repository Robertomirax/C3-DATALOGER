//---------------- main.c ---------------------------
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_vendor.h" // IWYU pragma: keep.
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

//#include <stdio.h>
#include <string.h>

#include "driver/uart.h"


static const char *TAG = "LVGL_APP";

// --- PINES ESP32-C3 SUPERMINI ---
#define LCD_HOST          SPI2_HOST
#define PIN_NUM_SCLK      4
#define PIN_NUM_MOSI      6
#define PIN_NUM_LCD_DC    2
#define PIN_NUM_LCD_RST   3
#define PIN_NUM_LCD_CS    -1
#define PIN_NUM_BK_LIGHT  1

// Definición de periféricos y pines
#define UART_PORT_NUM      UART_NUM_1
#define UART_TX_PIN        GPIO_NUM_21
#define UART_RX_PIN        GPIO_NUM_20
#define UART_BUF_SIZE      1024

// LED integrado en el ESP32-C3 Super Mini (Lógica invertida)
#define BLINK_GPIO         GPIO_NUM_8

#define LCD_H_RES         240
#define LCD_V_RES         240

static lv_disp_t *lvgl_disp = NULL;
static lv_obj_t *lbl_status = NULL; // Variable global para actualizar el texto en pantalla

void init_hardware(void)
{
    // 1. Configurar LED de estado
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BLINK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BLINK_GPIO, 1); // Apagado por defecto

    // 2. Parámetros de configuración de la UART
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT, // En ESP-IDF v6 asigna el reloj óptimo para C3
    };

    // 3. Aplicar configuración al puerto UART1
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));

    // 4. Mapear los pines GPIO a la UART1
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, 
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 5. Instalar el driver de UART asignando buffers RX/TX
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART1 inicializada correctamente en GPIO20 (RX) y GPIO21 (TX)");
}

// Tarea para leer datos entrantes desde el puerto RS-232
static void rx_task(void *arg)
{
    uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);

    while (1) {
        // Leer datos del buffer de la UART con timeout de 20ms
        int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(20));

        if (rxBytes > 0) {
            data[rxBytes] = '\0'; // Asegurar terminación de cadena
            ESP_LOGI(TAG, "Recibido vía RS-232 (%d bytes): %s", rxBytes, data);

            // Actualizar la interfaz de LVGL de forma segura
            if (lbl_status != NULL) {
                if (lvgl_port_lock(0)) {
                    // Actualizar el texto del label con el dato recibido
                    lv_label_set_text_fmt(lbl_status, "Recibido: %s", (char *)data);
                    lvgl_port_unlock();
                }
            }

            // Destello del LED al recibir datos
            gpio_set_level(BLINK_GPIO, 0); // Enciende LED
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(BLINK_GPIO, 1); // Apaga LED
        }
    }
    free(data);
    vTaskDelete(NULL);
}

// Tarea de prueba para enviar un mensaje cada 5 segundos
static void tx_task(void *arg)
{
    const char *test_msg = "Hola desde ESP32-C3 Super Mini (ESP-IDF v6)\r\n";
    
    while (1) {
        uart_write_bytes(UART_PORT_NUM, test_msg, strlen(test_msg));
        ESP_LOGI(TAG, "Mensaje enviado a puerto RS-232");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


void create_ui(void)
{
    // IMPORTANTE: Siempre bloquear LVGL al modificar la interfaz desde una tarea externa
    if (lvgl_port_lock(0)) {
        
        // Obtener la pantalla activa
        lv_obj_t *scr = lv_disp_get_scr_act(lvgl_disp);
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

        // 1. TÍTULO PRINCIPAL
        lv_obj_t *lbl_title = lv_label_create(scr);
        lv_label_set_text(lbl_title, "DATALOGGER");
        lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_30, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 0);

       
        // 3. ETIQUETA DE DATOS UART (Variable global)
        lbl_status = lv_label_create(scr);
        lv_label_set_text(lbl_status, "Esperando datos...");
        lv_obj_set_style_text_color(lbl_status, lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_28, 0);
        lv_obj_set_width(lbl_status, 230); // Ajustar ancho para permitir ajuste de línea
        lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP); // Salto de línea automático
        lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Liberar el bloqueo de LVGL
        lvgl_port_unlock();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Encendido de Backlight...");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(PIN_NUM_BK_LIGHT, 1);

    ESP_LOGI(TAG, "Iniciando Bus SPI...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Configurando IO del Panel ST7789...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000, // 20 MHz para animación fluida en LVGL
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,               // Modo 3 imprescindible para tu pantalla
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // --- INICIALIZACIÓN DE LVGL ---
    ESP_LOGI(TAG, "Inicializando LVGL Port...");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 20, // Buffer en RAM
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        }
    };
    
    // Registrar el panel en LVGL
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    ESP_LOGI(TAG, "Creando Interfaz de Usuario...");
    create_ui();

    init_hardware();

    // Crear tareas concurrentes en FreeRTOS
    xTaskCreate(rx_task, "uart_rx_task", 3072, NULL, 5, NULL);
    xTaskCreate(tx_task, "uart_tx_task", 3072, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}