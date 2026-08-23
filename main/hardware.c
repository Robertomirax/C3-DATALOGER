//------------------------------ hardware.c --------------------------------

#include "hardware.h"
#include "driver/gpio.h" // IWYU pragma: keep
#include "driver/uart.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include <stddef.h>

static const char *TAG = "HARDWARE";

void hardware_init_uart(void) {
  uart_config_t uart_config = {
      .baud_rate = BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(
      uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));

  ESP_LOGI(TAG, "UART1 inicializada en GPIO20 (RX) y GPIO21 (TX)");
}

// Configurar y montar LittleFS
static esp_err_t init_littlefs(void) {
  ESP_LOGI(TAG, "Inicializando LittleFS");

  esp_vfs_littlefs_conf_t conf = {.base_path = "/archivos",
                                  .partition_label = "archivos",
                                  .format_if_mount_failed = true,
                                  .dont_mount = false};

  esp_err_t ret = esp_vfs_littlefs_register(&conf);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Fallo al montar o formatear LittleFS");
    } else {
      ESP_LOGE(TAG, "Error al inicializar LittleFS (%s)", esp_err_to_name(ret));
    }
    return ret;
  }

  size_t total = 0, used = 0;
  ret = esp_littlefs_info(conf.partition_label, &total, &used);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "LittleFS Montado. Total: %d, Usado: %d", total, used);
  }
  return ESP_OK;
}

void hardware_init_all(void) {
  ESP_ERROR_CHECK(init_littlefs());

  hardware_init_uart();
}