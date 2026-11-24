// UART link to the ESP32 on the Flipper's GPIO USART (pin 13 TX, pin 14 RX), 115200 8N1.
#pragma once
#include <furi.h>
#include <furi_hal_serial.h>

typedef struct Uart Uart;

Uart* uart_open(void);
void uart_close(Uart* u);

// Send one command line (a '\n' is appended).
void uart_send(Uart* u, const char* line);
void uart_send_raw(Uart* u, const uint8_t* data, size_t len);

// Read one '\n'-terminated line (without the newline) into buf, up to buf_sz-1.
// Returns the length, or -1 on timeout.
int uart_read_line(Uart* u, char* buf, size_t buf_sz, uint32_t timeout_ms);

// Read exactly len raw bytes (for file transfers). Returns bytes read.
size_t uart_read_bytes(Uart* u, uint8_t* buf, size_t len, uint32_t timeout_ms);

// Read a single byte from the command channel; returns 0-255, or -1 on timeout.
int uart_read_byte(Uart* u, uint32_t timeout_ms);

// Drop anything buffered on the command channel (call before a request).
// Does NOT touch the serial-monitor log capture.
void uart_flush(Uart* u);

// Non-blocking drain of the serial-monitor capture (every RX byte, regardless of
// uart_flush). Returns bytes copied into buf (0 if none waiting).
size_t uart_read_log(Uart* u, uint8_t* buf, size_t max);
