//----------------------------   hardware.h ----------------

#ifndef HARDWARE_H
#define HARDWARE_H

#ifdef __cplusplus
extern "C" {
#endif

// --- CONFIGURACIÓN UART / MAX3232 ---
#define UART_PORT_NUM UART_NUM_1
#define UART_TX_PIN GPIO_NUM_21
#define UART_RX_PIN GPIO_NUM_20
#define UART_BUF_SIZE 1024
#define BAUD_RATE 4800

// --- OTROS PINES ---
// #define BLINK_GPIO        GPIO_NUM_8

void hardware_init_gpio(void);
void hardware_init_uart(void);
void hardware_init_all(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H