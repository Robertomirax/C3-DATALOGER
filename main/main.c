//---------------------- main.c ------------------

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MAIN_APP";

// Header inicial
static const char DL_HEADER[] = "\r\n\r\nAlert Technologies\r\nDATALOGGER "
                                "VER1.04\r\n\r\nMemory Used...08%\r\n";

// Tarea para enviar el archivo por UART
static void tx_file_task(void *arg) {
  const char *file_path = "/archivos/log_uart.txt";

  FILE *f = fopen(file_path, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Error al abrir el archivo para lectura: %s", file_path);
    vTaskDelete(NULL);
    return;
  }

  uint8_t *tx_buffer = (uint8_t *)malloc(UART_BUF_SIZE);
  if (tx_buffer == NULL) {
    ESP_LOGE(TAG, "Error asignando RAM para TX buffer");
    fclose(f);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Transmitiendo archivo...");

  size_t bytes_read = 0;
  size_t total_sent = 0;

  while ((bytes_read = fread(tx_buffer, 1, UART_BUF_SIZE, f)) > 0) {
    int bytes_sent =
        uart_write_bytes(UART_PORT_NUM, (const char *)tx_buffer, bytes_read);
    if (bytes_sent > 0) {
      total_sent += bytes_sent;
    }
    uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(100));
  }

  ESP_LOGI(TAG, "Transmisión finalizada. %zu bytes enviados", total_sent);

  free(tx_buffer);
  fclose(f);
  vTaskDelete(NULL);
}

// Tarea RX
static void rx_task(void *arg) {
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE + 1);

  if (data == NULL) {
    ESP_LOGE(TAG, "Error asignando memoria RAM para RX");
    vTaskDelete(NULL);
    return;
  }

  const char *log_path = "/archivos/log_uart.txt";
  bool esperando_confirmacion = false;

  while (1) {
    int rxBytes =
        uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      data[rxBytes] = '\0'; // Null-terminate seguro

      // Imprime solo los bytes exactos recibidos por la terminal de depuración
      // (IDF Log)
      ESP_LOGI(TAG, "Recibido (%d bytes): %.*s", rxBytes, rxBytes,
               (char *)data);

      // Imprime una tabla estructurada en Hexadecimal y ASCII
      ESP_LOG_BUFFER_HEXDUMP(TAG, data, rxBytes, ESP_LOG_INFO);

      // --- ESTADO DE ESPERA DE CONFIRMACIÓN ---
      if (esperando_confirmacion) {
        // Evaluar el primer carácter recibido ignorando espacios
        char resp = data[0];
        if (resp == 'y' || resp == 'Y') {

          const char *msg1 = "Self Diag ...Waiting\r\n";
            uart_write_bytes(UART_PORT_NUM, msg1, strlen(msg1));
            

          FILE *f = fopen(log_path, "w");
          if (f != NULL) {
            fclose(f);
            ESP_LOGI(TAG, "Archivo borrado exitosamente.");
            const char *msg = "RAM test successful\r\n\r\nDone\r\n";
            uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));
          } else {
            ESP_LOGE(TAG, "RAM test failed \r\n\r\nDone\r\n");
            const char *msg = "\r\n[ERROR] Fallo al borrar el archivo.\r\n";
            uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));
          }
        } else {
          ESP_LOGI(TAG, "Borrado cancelado por el usuario.");
          const char *msg = "\r\n[INFO] Operacion cancelada.\r\n";
          uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));
        }

        esperando_confirmacion = false;
      }
      // --- FLUJO NORMAL DE COMANDOS / DATOS ---
      else {
        // 1. COMANDO "enviar"
        if (strstr((char *)data, "send") != NULL) {
          ESP_LOGI(TAG, "Comando 'send' recibido!");

          const char *cambio = "ChangeBaud->4800 in 10Sec\r\n";
          uart_write_bytes(UART_PORT_NUM, cambio, strlen(cambio));
          // Esperar a que el mensaje termine de transmitirse antes de pausar
          uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(500));

          // --- RETARDO DE 10 SEGUNDOS ---
          ESP_LOGI(TAG, "Iniciando espera de 10 segundos...");
          vTaskDelay(pdMS_TO_TICKS(10000));

          // Cambiar la velocidad de la UART a 4800 baudios
          esp_err_t err = uart_set_baudrate(UART_PORT_NUM, 4800);
          if (err == ESP_OK) {
            ESP_LOGI(TAG, "Baudrate cambiado exitosamente a 4800");
          } else {
            ESP_LOGE(TAG, "Error al cambiar el baudrate: %s",
                     esp_err_to_name(err));
          }

          if (xTaskGetHandle("tx_file_task") == NULL) {
            xTaskCreate(tx_file_task, "tx_file_task", 4096, NULL, 5, NULL);
          } else {
            ESP_LOGW(TAG, "Transferencia previa aun en progreso.");
          }
        }
        // 2. COMANDO "borrar"
        else if (strstr((char *)data, "mtest") != NULL) {
          ESP_LOGW(TAG,
                   "Solicitud de borrado recibida. Pidiendo confirmacion...");
          esperando_confirmacion = true;

          const char *prompt = "Will clear data\r\nAre you sure? y/n  \r\n";
          uart_write_bytes(UART_PORT_NUM, prompt, strlen(prompt));
        }
        // 3. GUARDAR REGISTRO REGULAR
        else {
          FILE *f = fopen(log_path, "a");
          if (f == NULL) {
            ESP_LOGE(TAG, "Error al abrir el archivo log_uart.txt");
          } else {
            size_t written = fwrite(data, 1, rxBytes, f);
            fclose(f);
            ESP_LOGI(TAG, "Guardados %zu bytes en el archivo", written);
          }
        }
      }

    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  free(data);
  vTaskDelete(NULL);
}

void app_main(void) {
  ESP_LOGI(TAG, "Inicializando Hardware...");
  hardware_init_all();

  // Lanza tarea de lectura UART
  xTaskCreate(rx_task, "uart_rx_task", 8192, NULL, 5, NULL);

  // Envía encabezado
  uart_write_bytes(UART_PORT_NUM, DL_HEADER, strlen(DL_HEADER));

  vTaskDelete(NULL);
}