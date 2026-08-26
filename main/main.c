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

bool esperando_confirmacion = false;

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
static int linea = 0x00;

// ---------------------------------------------------------------------------
// Manejo del Buffer de Registro en RAM
// ---------------------------------------------------------------------------
static char line_buf[128];
static size_t line_idx = 0;
static size_t pos_flag_tara = 0;

static void reset_line_buffer(void) {
  line_idx = 0;
  pos_flag_tara = 0;
  memset(line_buf, 0, sizeof(line_buf));
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

    case WAIT_CRLF_1:
      if (last_byte == 0x0D && b == 0x0A) {
        ESP_LOGI(TAG, "CRLF detectado. Parseando trama en RAM...");
        reset_line_buffer();
        append_str_to_line("\r\n");
        pos_flag_tara = line_idx;
        append_str_to_line("0 ");
        subestado_loop = WAIT_COMMA_1;
      }
      break;

    case WAIT_COMMA_1:
      if (b == ',') {
        subestado_loop = TRIM_ZEROS_2;
      }
      break;

    case TRIM_ZEROS_2:
      if (b == ',') {
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_2;
      }
      break;

    case CAPTURE_TO_COMMA_2:
      if (b == ',') {
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else {
        append_char_to_line((char)b);
      }
      break;

    case IGNORE_TO_COMMA_3:
      if (b == ',') {
        subestado_loop = TRIM_ZEROS_4;
      }
      break;

    case TRIM_ZEROS_4:
      if (b == ',') {
        subestado_loop = WAIT_TARE;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_4;
      }
      break;

    case CAPTURE_TO_COMMA_4:
      if (b == ',') {
        subestado_loop = WAIT_TARE;
      } else {
        append_char_to_line((char)b);
      }
      break;

    case WAIT_TARE:

      if (b != '0' && b != '1') {
        break;
      }

      ESP_LOGI(TAG, "Byte Tare: 0x%02X", b);
      if (b == '1') {
        if (pos_flag_tara < line_idx) {
          line_buf[pos_flag_tara] = '1';
        }
      }

      // Escritura atómica al disco al completar la secuencia
      if (line_idx > 0 && f != NULL) {
        fwrite(line_buf, 1, line_idx, f);
        fflush(f);
        ESP_LOGI(TAG, "Guardado en Flash: %s", line_buf);
      }

      subestado_loop = WAIT_CRLF_1;

      linea++;
      if ((linea) % 256 == 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\r\nSeq#..%05d", (linea));
        fwrite(buffer, sizeof(char), strlen(buffer), f);
        fflush(f);
        ESP_LOGI(TAG, "Se agregó la línea: %s", buffer);
      }

      break;
    }

    last_byte = b;
  }
}

// ---------------------------------------------------------------------------
// Tareas
// ---------------------------------------------------------------------------
void console_keyboard_task(void *arg) {
  ESP_LOGI(TAG_KB, "Monitoreo de teclado iniciado.");

  while (1) {
    int c = getchar();

    if (c == 'b' || c == 'B') {
      printf("\r\n[ALERTA] Solicitud de borrado.\r\nWill clear data\r\nAre you "
             "sure? y/n \r\n");
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
              ESP_LOGI(TAG_KB, "Archivo borrado.");
              printf("RAM test successful\r\n\r\nDone\r\n");
            } else {
              ESP_LOGE(TAG_KB, "Fallo al borrar.");
            }
          } else {
            ESP_LOGI(TAG_KB, "Operacion cancelada.");
          }
          esperando = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void tx_file_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(10000));

  esp_err_t err = uart_set_baudrate(UART_PORT_NUM, 4800);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Baudrate cambiado a 4800");
  }

  FILE *f = fopen(log_path, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Error abriendo archivo log");
    vTaskDelete(NULL);
    return;
  }

  uint8_t *tx_buffer = (uint8_t *)malloc(UART_BUF_SIZE);
  if (tx_buffer != NULL) {
    size_t bytes_read = 0;
    while ((bytes_read = fread(tx_buffer, 1, UART_BUF_SIZE, f)) > 0) {
      uart_write_bytes(UART_PORT_NUM, (const char *)tx_buffer, bytes_read);
      uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(100));
    }
    free(tx_buffer);
  }

  fclose(f);
  vTaskDelete(NULL);
}

static void rx_task(void *arg) {
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE + 1);
  if (data == NULL) {
    vTaskDelete(NULL);
    return;
  }

  bool inicio_transmision = true;
  uint8_t estado_grabado = 0;

  const char *target =
      "seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
  size_t target_len = strlen(target);

  char stream_buf[UART_BUF_SIZE + 32];
  size_t overlap_len = 0;

  while (1) {
    int rxBytes =
        uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      ESP_LOG_BUFFER_HEXDUMP(TAG, data, rxBytes, ESP_LOG_WARN);

      memcpy(stream_buf + overlap_len, data, rxBytes);
      size_t total_stream_len = overlap_len + rxBytes;
      stream_buf[total_stream_len] = '\0';

      uint16_t valor = (data[0] << 4) | data[1];

      // 1. Respuesta al cabezal inicial (SIN desechar data ni hacer continue)
      if (valor > 0x0F && inicio_transmision) {
        uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));
        inicio_transmision = false;
        overlap_len = 0;
      }

      // 2. Control de comandos especiales de UART
      if (esperando_confirmacion) {
        char resp = data[0];
        if (resp == 'y' || resp == 'Y') {
          uart_write_bytes(UART_PORT_NUM, "Self Diag ...Waiting\r\n", 22);
          ESP_LOGW(TAG, "Borrando");

          FILE *f = fopen(log_path, "w");
          if (f != NULL) {
            fclose(f);
            uart_write_bytes(UART_PORT_NUM,
                             "RAM test successful\r\n\r\nDone\r\n", 29);
          }
        }
        esperando_confirmacion = false;
      } else if (strstr(stream_buf, "send") != NULL) {
        const char *cambio = "ChangeBaud->4800 in 10Sec\r\n";
        uart_write_bytes(UART_PORT_NUM, cambio, strlen(cambio));

        if (xTaskGetHandle("tx_file_task") == NULL) {
          xTaskCreate(tx_file_task, "tx_file_task", 4096, NULL, 5, NULL);
        }
      } else if (strstr(stream_buf, "mtest") != NULL) {
        esperando_confirmacion = true;
        const char *prompt = "Will clear data\r\nAre you sure? y/n  \r\n";
        uart_write_bytes(UART_PORT_NUM, prompt, strlen(prompt));
      }

      // 3. GARANTÍA DE GRABACIÓN: Todos los datos entran al archivo de log
      FILE *f = fopen(log_path, "a+");
      if (f != NULL) {
        if (estado_grabado == 0) {
          char *match = (char *)memmem(stream_buf, total_stream_len, target,
                                       target_len);

          if (match != NULL) {
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
            // Escribe la ráfaga completa en el archivo
            fwrite(data, 1, rxBytes, f);

            // Copia no destructiva solo para visualización en log
            char *temp_data = (char *)malloc(rxBytes + 1);
            if (temp_data != NULL) {
              memcpy(temp_data, data, rxBytes);
              temp_data[rxBytes] = '\0';

              char *line = strtok(temp_data, "\r\n");
              while (line != NULL) {
                ESP_LOGI(TAG, "%s", line);
                line = strtok(NULL, "\r\n");
              }
              free(temp_data);
            }
          }
        } else if (estado_grabado == 2) {
          procesar_loop_secuencia(data, rxBytes, f);
        }
        fclose(f);
      }

      // 4. Mantenimiento del solapamiento del búfer
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