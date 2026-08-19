
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_vendor.h" // IWYU pragma: keep.
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"


static const char *TAG = "LVGL_APP";

// --- PINES ESP32-C3 SUPERMINI ---
#define LCD_HOST          SPI2_HOST
#define PIN_NUM_SCLK      4
#define PIN_NUM_MOSI      6
#define PIN_NUM_LCD_DC    2
#define PIN_NUM_LCD_RST   3
#define PIN_NUM_LCD_CS    -1
#define PIN_NUM_BK_LIGHT  1

#define LCD_H_RES         240
#define LCD_V_RES         240

static lv_disp_t *lvgl_disp = NULL;

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
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 20);

        // 2. SUBTÍTULO
        lv_obj_t *lbl_sub = lv_label_create(scr);
        lv_label_set_text(lbl_sub, "ESP32-C3 + LVGL");
        lv_obj_set_style_text_color(lbl_sub, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, -20);

        // 3. BARRA DE PROGRESO DE EJEMPLO
        lv_obj_t *bar = lv_bar_create(scr);
        lv_obj_set_size(bar, 180, 15);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_value(bar, 75, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);

        // 4. ETIQUETA DE ESTADO
        lv_obj_t *lbl_status = lv_label_create(scr);
        lv_label_set_text(lbl_status, "Estado: Sistema OK");
        lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
        lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -20);

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
        .buffer_size = LCD_H_RES * 40, // Buffer en RAM
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

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}