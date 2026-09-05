#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"


#include "driver/uart.h"
#include "esp_log.h"
#include "hardware.h"

// #include "lv_conf_internal.h"
#include "font/lv_font.h"

// ---------------------------------------------------------------------------
// Constantes y Definiciones
// ---------------------------------------------------------------------------
static const char *TAG = "MAIN_APP";
static const char *TAG_KB = "TECLADO";
static const char *TAG_FLASH = "FLASH_WRITER";
static const char *log_path = "/archivos/log_uart.txt";

//static const char DL_HEADER1[] = "\x55\x55\x55\x55\x55\x55\x55";
static const char DL_HEADER2[] = "\r\n\r\nAlert Technologies\r\nDATALOGGER "
                                 "VER1.04\r\n\nMemory Used...08%\r\n";

#define RING_BUF_SIZE (8 * 1024)   // Búfer circular de 8 KB en RAM
#define FLASH_WRITE_THRESHOLD 2048 // Escribir a Flash al acumular 2 KB
#define TEMP_WRITE_BUF_SIZE 2048

// ---------------------------------------------------------------------------
// Estructura del Búfer Circular (Ring Buffer Thread-Safe)
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t buffer[RING_BUF_SIZE];
  size_t head;
  size_t tail;
  size_t count;
  SemaphoreHandle_t mutex;
} ring_buffer_t;

static ring_buffer_t rb;

static void ring_buffer_init(void) {
  rb.head = 0;
  rb.tail = 0;
  rb.count = 0;
  rb.mutex = xSemaphoreCreateMutex();
}

static size_t ring_buffer_write(const uint8_t *data, size_t len) {
  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) != pdTRUE)
    return 0;

  size_t bytes_written = 0;
  for (size_t i = 0; i < len; i++) {
    if (rb.count < RING_BUF_SIZE) {
      rb.buffer[rb.head] = data[i];
      rb.head = (rb.head + 1) % RING_BUF_SIZE;
      rb.count++;
      bytes_written++;
    } else {
      ESP_LOGW(TAG, "Ring Buffer Lleno! Se descartaron bytes.");
      break;
    }
  }

  xSemaphoreGive(rb.mutex);
  return bytes_written;
}

static size_t ring_buffer_read(uint8_t *out_buf, size_t max_len) {
  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) != pdTRUE)
    return 0;

  size_t bytes_read = 0;
  while (bytes_read < max_len && rb.count > 0) {
    out_buf[bytes_read] = rb.buffer[rb.tail];
    rb.tail = (rb.tail + 1) % RING_BUF_SIZE;
    rb.count--;
    bytes_read++;
  }

  xSemaphoreGive(rb.mutex);
  return bytes_read;
}

static size_t ring_buffer_get_count(void) {
  size_t count = 0;
  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) == pdTRUE) {
    count = rb.count;
    xSemaphoreGive(rb.mutex);
  }
  return count;
}

// ---------------------------------------------------------------------------
// Variables Globales de Estado y UI
// ---------------------------------------------------------------------------
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *bar_status = NULL;

static bool esperando_confirmacion = false;
static bool transmitiendo_archivo = false;

// Mutex para evitar colisiones de lectura/escritura/borrado en el archivo
static SemaphoreHandle_t file_mutex = NULL;

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

// Buffer de Registro de Línea en RAM
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
static void procesar_loop_secuencia(uint8_t *buf, size_t len) {
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
      if (b == ',')
        subestado_loop = TRIM_ZEROS_2;
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
      if (b == ',')
        subestado_loop = TRIM_ZEROS_4;
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
      if (b != '0' && b != '1')
        break;

      ESP_LOGI(TAG, "Byte Tare: 0x%02X", b);
      if (b == '1' && pos_flag_tara < line_idx) {
        line_buf[pos_flag_tara] = '1';
      }

      // Depositar línea procesada en el Ring Buffer de RAM
      if (line_idx > 0) {
        ring_buffer_write((uint8_t *)line_buf, line_idx);
        ESP_LOGI(TAG, "Añadido a RAM Ring Buffer: %s", line_buf);
      }

      subestado_loop = WAIT_CRLF_1;
      linea++;

      if ((linea) % 256 == 0) {
        char seq_buf[32];
        int seq_len = snprintf(seq_buf, sizeof(seq_buf), "\r\nSeq#..%05d", linea);
        ring_buffer_write((uint8_t *)seq_buf, seq_len);
        ESP_LOGI(TAG, "Añadida línea de control a RAM: %s", seq_buf);
      }
      break;
    }
    last_byte = b;
  }
}

// ---------------------------------------------------------------------------
// Tareas Secundarias (Flash Writer & UI Monitor)
// ---------------------------------------------------------------------------
static void flash_writer_task(void *arg) {
  uint8_t *write_buf = (uint8_t *)malloc(TEMP_WRITE_BUF_SIZE);
  if (!write_buf) {
    ESP_LOGE(TAG_FLASH, "Fallo al asignar memoria para write_buf");
    vTaskDelete(NULL);
    return;
  }

  TickType_t last_flush_time = xTaskGetTickCount();

  while (1) {
    size_t pending_bytes = ring_buffer_get_count();
    TickType_t elapsed = xTaskGetTickCount() - last_flush_time;

    // Criterio de volcado: 2 KB acumulados O transcurridos 5 segundos
    bool should_write = (pending_bytes >= FLASH_WRITE_THRESHOLD) || (pending_bytes > 0 && elapsed >= pdMS_TO_TICKS(5000));

    if (should_write && !transmitiendo_archivo) {
      if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        FILE *f = fopen(log_path, "a+");
        if (f != NULL) {
          while (ring_buffer_get_count() > 0) {
            size_t bytes_to_read = ring_buffer_read(write_buf, TEMP_WRITE_BUF_SIZE);
            if (bytes_to_read > 0) {
              fwrite(write_buf, 1, bytes_to_read, f);
            }
          }
          fflush(f);
          fclose(f);
          ESP_LOGI(TAG_FLASH, "Volcado de RAM a Flash realizado correctamente.");
        } else {
          ESP_LOGE(TAG_FLASH, "Error abriendo archivo log.");
        }
        xSemaphoreGive(file_mutex);
      }
      last_flush_time = xTaskGetTickCount();
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }

  free(write_buf);
  vTaskDelete(NULL);
}

static void ui_update_task(void *arg) {
  char buf[64];

  while (1) {
    size_t pending_bytes = ring_buffer_get_count();
    uint32_t percentage = (pending_bytes * 100) / RING_BUF_SIZE;

    if (lvgl_port_lock(portMAX_DELAY)) {
      // Actualización de texto explicativo
      if (lbl_status != NULL) {
        snprintf(buf, sizeof(buf), "RAM Buffer: %u / %d B (%u%%)", (unsigned int)pending_bytes, RING_BUF_SIZE, (unsigned int)percentage);
        lv_label_set_text(lbl_status, buf);
      }

      // Actualización gráfica de la barra de progreso
      if (bar_status != NULL) {
        if (percentage > 80) {
          lv_obj_set_style_bg_color(bar_status, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
        } else {
          lv_obj_set_style_bg_color(bar_status, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
        }

        lv_bar_set_value(bar_status, percentage, LV_ANIM_ON);
      }

      lvgl_port_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }

  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Tareas Principales
// ---------------------------------------------------------------------------
void console_keyboard_task(void *arg) {
  ESP_LOGI(TAG_KB, "Monitoreo de teclado iniciado.");

  while (1) {
    int c = getchar();

    if (c == 'b') {
      printf("\r\n[ALERTA] Solicitud de borrado.\r\nWill clear data\r\nAre you sure? y/n \r\n");
      fflush(stdout);

      bool esperando = true;
      while (esperando) {
        int resp = getchar();
        if (resp != EOF && resp != '\r' && resp != '\n') {
          if (resp == 'y' || resp == 'Y') {
            printf("\r\nSelf Diag ...Waiting\r\n");

            if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
              FILE *f = fopen(log_path, "w");
              if (f != NULL) {
                fclose(f);
                ESP_LOGI(TAG_KB, "Archivo borrado.");
                printf("RAM test successful\r\n\r\nDone\r\n");
              } else {
                ESP_LOGE(TAG_KB, "Fallo al borrar.");
              }
              xSemaphoreGive(file_mutex);
            }
          } else {
            ESP_LOGI(TAG_KB, "Operacion cancelada.");
          }
          esperando = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
    if (c == 'e') {
      gpio_set_level(PIN_NUM_BK_LIGHT, 1); // Enciende display
    }
    if (c == 'a') {
      gpio_set_level(PIN_NUM_BK_LIGHT, 0); // Apaga display
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void tx_file_task(void *arg) {
  transmitiendo_archivo = true;

  vTaskDelay(pdMS_TO_TICKS(10000));

  if (uart_set_baudrate(UART_PORT_NUM, 4800) == ESP_OK) {
    ESP_LOGI(TAG, "Baudrate cambiado a 4800");
  }

  if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    FILE *f = fopen(log_path, "r");
    if (f != NULL) {
      uint8_t *tx_buffer = (uint8_t *)malloc(UART_BUF_SIZE);
      if (tx_buffer != NULL) {
        size_t bytes_read = 0;
        while ((bytes_read = fread(tx_buffer, 1, UART_BUF_SIZE, f)) > 0) {
          uart_write_bytes(UART_PORT_NUM, (const char *)tx_buffer, bytes_read);
          uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1000));
        }
        free(tx_buffer);
      }
      fclose(f);
    } else {
      ESP_LOGE(TAG, "Error abriendo archivo log para lectura");
    }
    xSemaphoreGive(file_mutex);
  }

  const char *msg = "\r\n\r\nChangeBaud->300\r\n\r\n";
  uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));
  uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(500));

  uart_set_baudrate(UART_PORT_NUM, 300);

  transmitiendo_archivo = false;
  vTaskDelete(NULL);
}

static void rx_task(void *arg) {
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
  char *stream_buf = (char *)malloc(UART_BUF_SIZE + 64);

  if (data == NULL || stream_buf == NULL) {
    ESP_LOGE(TAG, "Error asignando memoria estática para rx_task");
    if (data)
      free(data);
    if (stream_buf)
      free(stream_buf);
    vTaskDelete(NULL);
    return;
  }

  uint8_t estado_grabado = 0;
  const char *target = "seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
  size_t target_len = strlen(target);

  char overlap_buf[64] = {0};
  size_t overlap_len = 0;

  while (1) {
    int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      ESP_LOG_BUFFER_HEXDUMP(TAG, data, rxBytes, ESP_LOG_WARN);

      bool ignorar_grabacion = false;
      size_t total_stream_len = overlap_len + rxBytes;

      memcpy(stream_buf, overlap_buf, overlap_len);
      memcpy(stream_buf + overlap_len, data, rxBytes);
      stream_buf[total_stream_len] = '\0';

      // 1. Manejo de Comandos RS-232
      if (esperando_confirmacion) {
        char resp = data[0];
        if (resp == 'y' || resp == 'Y') {
          uart_write_bytes(UART_PORT_NUM, "Self Diag ...Waiting\r\n", 22);

          if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            FILE *f_clr = fopen(log_path, "w");
            if (f_clr != NULL) {
              fclose(f_clr);
              uart_write_bytes(UART_PORT_NUM, "RAM test successful\r\n\r\nDone\r\n", 29);
            }
            xSemaphoreGive(file_mutex);
          }
        }
        esperando_confirmacion = false;
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));

      } else if (strstr(stream_buf, "send") != NULL) {
        if (!transmitiendo_archivo) {
          const char *cambio = "ChangeBaud->4800 in 10Sec\r\n";
          uart_write_bytes(UART_PORT_NUM, cambio, strlen(cambio));
          xTaskCreate(tx_file_task, "tx_file_task", 4096, NULL, 5, NULL);
        }
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));

      } else if (strstr(stream_buf, "mtest") != NULL) {
        esperando_confirmacion = true;
        const char *prompt = "Will clear data\r\nAre you sure? y/n  \r\n";
        uart_write_bytes(UART_PORT_NUM, prompt, strlen(prompt));
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));
      }

      // 2. Procesamiento de Cabecera y Trama de Datos
      if (!ignorar_grabacion && !transmitiendo_archivo) {
        if (estado_grabado == 0) {
          char *match = (char *)memmem(stream_buf, total_stream_len, target, target_len);

          if (match != NULL) {
            size_t stream_match_idx = match - stream_buf;
            size_t data_match_end_idx =
                (stream_match_idx >= overlap_len) ? (stream_match_idx - overlap_len + target_len) : (target_len - (overlap_len - stream_match_idx));

            // Guardar la cabecera en la RAM
            ring_buffer_write(data, data_match_end_idx);

            // Transición a la captura continua de tramas
            estado_grabado = 2;
            subestado_loop = WAIT_CRLF_1;
            last_byte = 0x00;

            // Procesar el resto del buffer inmediatamente en la máquina de estados
            if (rxBytes > data_match_end_idx) {
              procesar_loop_secuencia(data + data_match_end_idx, rxBytes - data_match_end_idx);
            }
          } else {
            // Guardar texto inicial antes de la cabecera
            ring_buffer_write(data, rxBytes);
          }
        } else if (estado_grabado == 2) {
          // Captura continua activa: procesar todos los bytes entrantes
          procesar_loop_secuencia(data, rxBytes);
        }
      }

      // 3. Mantenimiento del buffer de solapamiento
      if (!ignorar_grabacion && !transmitiendo_archivo) {
        if (total_stream_len >= (target_len - 1)) {
          overlap_len = target_len - 1;
          if (overlap_len > sizeof(overlap_buf)) {
            overlap_len = sizeof(overlap_buf);
          }
          memcpy(overlap_buf, stream_buf + total_stream_len - overlap_len, overlap_len);
        } else {
          overlap_len = total_stream_len;
          if (overlap_len > sizeof(overlap_buf)) {
            overlap_len = sizeof(overlap_buf);
          }
          memcpy(overlap_buf, stream_buf, overlap_len);
        }
      }
    }
  }

  free(data);
  free(stream_buf);
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Interfaz de Usuario LVGL
// ---------------------------------------------------------------------------
void create_ui(void) {
  if (lvgl_port_lock(portMAX_DELAY)) {
    lv_obj_t *scr = lv_disp_get_scr_act(lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Título Superior
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "DATALOGGER");
    lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_30, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 15);

    // Etiqueta de Estado de RAM
    lbl_status = lv_label_create(scr);
    lv_label_set_text(lbl_status, "Iniciando...");
    lv_obj_set_style_text_color(lbl_status, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_set_width(lbl_status, 220);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_30, 0);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, -10);

    // Widget Barra de Progreso (lv_bar)
    bar_status = lv_bar_create(scr);
    lv_obj_set_size(bar_status, 200, 15);
    lv_obj_align(bar_status, LV_ALIGN_CENTER, 0, 30);
    lv_bar_set_range(bar_status, 0, 100);
    lv_bar_set_value(bar_status, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(bar_status, lv_palette_darken(LV_PALETTE_GREY, 3), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_status, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);

    lvgl_port_unlock();
  }
}

// ---------------------------------------------------------------------------
// Entrada Principal (Main)
// ---------------------------------------------------------------------------
void app_main(void) {
  ESP_LOGI(TAG, "Inicializando Hardware y Estructuras de Memoria...");

  

  file_mutex = xSemaphoreCreateMutex();
  ring_buffer_init();

  hardware_init_all();
  ESP_LOGI(TAG, "Creando Interfaz de Usuario...");
  create_ui();

  //vTaskDelay(pdMS_TO_TICKS(100));

  // Creación de tareas FreeRTOS
  xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
  xTaskCreate(flash_writer_task, "flash_writer_task", 4096, NULL, 4, NULL);
  xTaskCreate(ui_update_task, "ui_update_task", 2048, NULL, 3, NULL);
  xTaskCreate(console_keyboard_task, "console_keyboard_task", 2048, NULL, 5, NULL);

  //vTaskDelay(pdMS_TO_TICKS(100));
 // uart_write_bytes(UART_PORT_NUM, DL_HEADER1, strlen(DL_HEADER1));
 // vTaskDelay(pdMS_TO_TICKS(50));
  uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));

  vTaskDelete(NULL);
}