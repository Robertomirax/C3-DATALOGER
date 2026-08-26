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
static const char *TAG_KB = "TECLADO";
static const char *log_path = "/archivos/log_uart.txt";

static const char DL_HEADER2[] = "\r\n\r\nAlert Technologies\r\nDATALOGGER "
                                 "VER1.04\r\n\nMemory Used...08%\r\n";

extern bool esperando_confirmacion;

typedef enum {
  WAIT_CRLF_1,
  WAIT_COMMA_1,
  TRIM_ZEROS_2,
  CAPTURE_TO_COMMA_2,
  IGNORE_TO_COMMA_3,
  TRIM_ZEROS_4,
  CAPTURE_TO_COMMA_4,
  WAIT_TARE
} subestado_loop_t;

static subestado_loop_t subestado_loop = WAIT_CRLF_1;
static uint8_t last_byte = 0x00;

// ---------------------------------------------------------------------------
// Manejo del Buffer de Registro en RAM
// ---------------------------------------------------------------------------
static char line_buf[128];
static size_t line_idx = 0;
static size_t pos_flag_tara =
    0; // Índice en line_buf donde reside el flag de tara ('0' inicial)

static void reset_line_buffer(void) {
  line_idx = 0;
  pos_flag_tara = 0;
}

static void append_char_to_line(char c) {
  if (line_idx < sizeof(line_buf) - 1) {
    line_buf[line_idx++] = c;
    line_buf[line_idx] = '\0';
  }
}

static void append_str_to_line(const char *str) {
  while (*str && (line_idx < sizeof(line_buf) - 1)) {
    line_buf[line_idx++] = *str++;
  }
  line_buf[line_idx] = '\0';
}

// ---------------------------------------------------------------------------
// Procesador byte a byte
// ---------------------------------------------------------------------------
static void procesar_loop_secuencia(uint8_t *buf, size_t len, FILE *f) {
  for (size_t i = 0; i < len; i++) {
    uint8_t b = buf[i];

    switch (subestado_loop) {

    // 1. Espera el primer 0x0D 0x0A
    case WAIT_CRLF_1:
      if (last_byte == 0x0D && b == 0x0A) {
        ESP_LOGI(TAG,
                 "Primer CRLF detectado. Inicializando registro en RAM...");
        reset_line_buffer();

        append_str_to_line("\r\n");
        pos_flag_tara = line_idx; // Almacenamos dónde se ubica el '0' inicial
        append_str_to_line("0 ");

        subestado_loop = WAIT_COMMA_1;
      }
      break;

    // 2. Espera la 1ª coma ','
    case WAIT_COMMA_1:
      if (b == ',') {
        ESP_LOGI(TAG, "1ª coma detectada. Omitiendo ceros iniciales...");
        subestado_loop = TRIM_ZEROS_2;
      }
      break;

    // 2.1 Descarta '0' y espacios iniciales de la 1ª captura
    case TRIM_ZEROS_2:
      if (b == ',') {
        ESP_LOGI(TAG, "2ª coma detectada. Agregando espacio...");
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_2;
      }
      break;

    // 3. Graba todo hasta encontrar la 2ª coma ','
    case CAPTURE_TO_COMMA_2:
      if (b == ',') {
        ESP_LOGI(TAG, "2ª coma detectada. Agregando espacio...");
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else {
        append_char_to_line((char)b);
      }
      break;

    // 4. Obvia todo lo recibido entre la 2ª y la 3ª coma
    case IGNORE_TO_COMMA_3:
      if (b == ',') {
        ESP_LOGI(
            TAG,
            "3ª coma detectada. Omitiendo ceros iniciales de 2ª captura...");
        subestado_loop = TRIM_ZEROS_4;
      }
      break;

    // 4.1 Descarta '0' y espacios iniciales de la 2ª captura
    case TRIM_ZEROS_4:
      if (b == ',') {
        ESP_LOGI(TAG, "4ª coma detectada.");
        subestado_loop = WAIT_TARE;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_4;
      }
      break;

    // 5. Graba todo hasta encontrar la 4ª coma ','
    case CAPTURE_TO_COMMA_4:
      if (b == ',') {
        ESP_LOGI(TAG, "4ª coma detectada. Pasando a evaluación de TARA...");
        subestado_loop = WAIT_TARE;
      } else {
        append_char_to_line((char)b);
      }
      break;

    // 5.1 Evalúa el byte de Tara, actualiza RAM y escribe en Flash
    case WAIT_TARE:
      ESP_LOGW(TAG, "Tara recibida = 0x%02X", b);
      if (b != '0' && b != '1')
      {
        break;
      }      

      if (b == '1' || b == 0x31) {
        // Modificación limpia directamente en el buffer de RAM
        if (pos_flag_tara < line_idx) {
          line_buf[pos_flag_tara] = '1';
        }
        ESP_LOGW(TAG, "Flag de Tara actualizado a '1' en RAM");
      }

      // Escritura atómica a disco de la línea final
      if (line_idx > 0) {
        fwrite(line_buf, 1, line_idx, f);
        fflush(f);
        ESP_LOGI(TAG, "Línea guardada exitosamente en Flash: %s", line_buf);
      }

      // Reinicia máquina de estados para la siguiente trama
      subestado_loop = WAIT_CRLF_1;
      break;
    }

    last_byte = b;
  }
}

// ---------------------------------------------------------------------------
// Tareas independientes
// ---------------------------------------------------------------------------
void console_keyboard_task(void *arg) {
  ESP_LOGI(TAG_KB, "Monitoreo de teclado iniciado. Presiona 'B' para borrar.");

  while (1) {
    int c = getchar();

    if (c != EOF) {
      if (c == 'b' || c == 'B') {
        printf("\r\n[ALERTA] Solicitud de borrado detectada.\r\n");
        printf("Will clear data\r\nAre you sure? y/n \r\n");
        fflush(stdout);

        bool esperando = true;
        while (esperando) {
          int resp = getchar();
          if (resp != EOF && resp != '\r' && resp != '\n') {
            if (resp == 'y' || resp == 'Y') {
              printf("\r\nSelf Diag ...Waiting\r\n");

              FILE *f = fopen(log_path, "w");
              if (f != NULL) {
                fclose(f);
                ESP_LOGI(TAG_KB, "Archivo borrado exitosamente.");
                printf("RAM test successful\r\n\r\nDone\r\n");
              } else {
                ESP_LOGE(TAG_KB, "RAM test failed.");
                printf("\r\n[ERROR] Fallo al borrar el archivo.\r\n");
              }
            } else {
              ESP_LOGI(TAG_KB, "Borrado cancelado por el usuario.");
              printf("\r\n[INFO] Operacion cancelada.\r\n");
            }
            esperando = false;
          }
          vTaskDelay(pdMS_TO_TICKS(20));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

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

static void rx_task(void *arg) {
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE + 1);

  if (data == NULL) {
    ESP_LOGE(TAG, "Error asignando memoria RAM para RX");
    vTaskDelete(NULL);
    return;
  }

  bool esperando_confirmacion = false;
  bool inicio_transmision = true;

  int estado_grabado = 0;

  const char *target =
      "seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
  size_t target_len = strlen(target);

  char stream_buf[UART_BUF_SIZE + 32];
  size_t overlap_len = 0;

  while (1) {
    int rxBytes =
        uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      data[rxBytes] = '\0';

      ESP_LOGI(TAG, "Recibido (%d bytes): %.*s", rxBytes, rxBytes,
               (char *)data);
      ESP_LOG_BUFFER_HEXDUMP(TAG, data, rxBytes, ESP_LOG_INFO);

      memcpy(stream_buf + overlap_len, data, rxBytes);
      size_t total_stream_len = overlap_len + rxBytes;
      stream_buf[total_stream_len] = '\0';

      uint16_t valor = (data[0] << 4) | data[1];

      if (valor > 0x0F && inicio_transmision) {
        ESP_LOGI(TAG, "Valor binario mayor a 0x0F recibido: 0x%02X", valor);
        uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));
        inicio_transmision = false;
        overlap_len = 0;
        continue;
      }

      if (esperando_confirmacion) {
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

      else if (strstr(stream_buf, "send") != NULL) {
        ESP_LOGI(TAG, "Comando 'send' recibido!");

        const char *cambio = "ChangeBaud->4800 in 10Sec\r\n";
        uart_write_bytes(UART_PORT_NUM, cambio, strlen(cambio));
        uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "Iniciando espera de 10 segundos...");
        vTaskDelay(pdMS_TO_TICKS(10000));

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

      else if (strstr(stream_buf, "mtest") != NULL) {
        ESP_LOGW(TAG,
                 "Solicitud de borrado recibida. Pidiendo confirmacion...");
        esperando_confirmacion = true;

        const char *prompt = "Will clear data\r\nAre you sure? y/n  \r\n";
        uart_write_bytes(UART_PORT_NUM, prompt, strlen(prompt));
      }

      else {
        // Apertura estándar con "a+" (append puro sin seek relativo)
        FILE *f = fopen(log_path, "a+");

        if (f == NULL) {
          ESP_LOGE(TAG, "Error al abrir el archivo log_uart.txt");
        } else {
          if (estado_grabado == 0) {
            char *match = (char *)memmem(stream_buf, total_stream_len, target,
                                         target_len);

            if (match != NULL) {
              ESP_LOGI(TAG, "Frase objetivo detectada. Entrando al bucle...");

              size_t stream_match_idx = match - stream_buf;
              size_t data_match_end_idx =
                  (stream_match_idx >= overlap_len)
                      ? (stream_match_idx - overlap_len + target_len)
                      : (target_len - (overlap_len - stream_match_idx));

              fwrite(data, 1, data_match_end_idx, f);

              estado_grabado = 2;
              subestado_loop = WAIT_CRLF_1;
              last_byte = 0x00;

              size_t bytes_restantes = rxBytes - data_match_end_idx;
              if (bytes_restantes > 0) {
                procesar_loop_secuencia(data + data_match_end_idx,
                                        bytes_restantes, f);
              }
            } else {
              fwrite(data, 1, rxBytes, f);
            }
          } else if (estado_grabado == 2) {
            procesar_loop_secuencia(data, rxBytes, f);
          }

          fclose(f);
        }
      }

      if (total_stream_len >= (target_len - 1)) {
        overlap_len = target_len - 1;
        memcpy(stream_buf, stream_buf + total_stream_len - overlap_len,
               overlap_len);
      } else {
        overlap_len = total_stream_len;
      }
    }
  }

  free(data);
  vTaskDelete(NULL);
}

void app_main(void) {
  ESP_LOGI(TAG, "Inicializando Hardware...");
  hardware_init_all();

  vTaskDelay(pdMS_TO_TICKS(100));

  xTaskCreate(rx_task, "uart_rx_task", 8192, NULL, 5, NULL);
  xTaskCreate(console_keyboard_task, "console_keyboard_task", 2048, NULL, 5,
              NULL);

  vTaskDelay(pdMS_TO_TICKS(50));
  uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));

  vTaskDelete(NULL);
}