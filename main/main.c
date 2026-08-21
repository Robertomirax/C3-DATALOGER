//---------------------- main.c ------------------

//#include <stdlib.h>
#include "freertos/FreeRTOS.h"  
#include "freertos/task.h"
#include "esp_log.h"
#include "hardware.h"
//#include "lvgl.h"
static const char *TAG = "MAIN_APP";
static lv_obj_t *lbl_status = NULL;

static void rx_task(void *arg)
{
    uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Error asignando memoria para RX");
        vTaskDelete(NULL);
    }

    while (1) {
        int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(50));

        if (rxBytes > 0) {
            data[rxBytes] = '\0';
            ESP_LOGI(TAG, "RS-232 (%d bytes): %s", rxBytes, data);

            if (lbl_status != NULL) {
                // Timeout de 100ms para asegurar la toma del mutex de LVGL
                if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
                    lv_label_set_text_fmt(lbl_status, "Recibido:\n%s", (char *)data);
                    lvgl_port_unlock();
                }
            }

            gpio_set_level(BLINK_GPIO, 0); 
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(BLINK_GPIO, 1); 
        }
    }
    free(data);
    vTaskDelete(NULL);
}

void create_ui(void)
{
    // Espera indeterminada segura para la creación de la UI
    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_obj_t *scr = lv_disp_get_scr_act(lvgl_disp);
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

        lv_obj_t *lbl_title = lv_label_create(scr);
        lv_label_set_text(lbl_title, "DATALOGGER");
        lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 15);

        lbl_status = lv_label_create(scr);
        lv_label_set_text(lbl_status, "Esperando datos RS-232...");
        lv_obj_set_style_text_color(lbl_status, lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_obj_set_width(lbl_status, 220);
        lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 10);

        lvgl_port_unlock();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando Hardware...");
    hardware_init_all();

    ESP_LOGI(TAG, "Creando Interfaz de Usuario...");
    create_ui();

    xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, 5, NULL);

    // Destruye el hilo principal para liberar la pila de app_main
    vTaskDelete(NULL);
}