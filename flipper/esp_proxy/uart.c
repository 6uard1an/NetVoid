#include "uart.h"
#include <furi_hal_serial_control.h>
#include <expansion/expansion.h>

#define RX_STREAM_SIZE 4096
#define LOG_STREAM_SIZE 8192 // holds serial between monitor drains, incl. during blocking ops

struct Uart {
    FuriHalSerialHandle* handle;
    FuriStreamBuffer* rx;
    FuriStreamBuffer* log; // every RX byte, for the serial monitor; survives uart_flush
    Expansion* expansion;
};

static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    Uart* u = ctx;
    if(event == FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(u->rx, &b, 1, 0);
        furi_stream_buffer_send(u->log, &b, 1, 0);
    }
}

Uart* uart_open(void) {
    Uart* u = malloc(sizeof(Uart));
    u->rx = furi_stream_buffer_alloc(RX_STREAM_SIZE, 1);
    u->log = furi_stream_buffer_alloc(LOG_STREAM_SIZE, 1);
    // The Expansion service owns the USART by default; release it while we drive the ESP32.
    u->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(u->expansion);
    u->handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_check(u->handle);
    furi_hal_serial_init(u->handle, 115200);
    furi_hal_serial_async_rx_start(u->handle, uart_rx_cb, u, false);
    return u;
}

void uart_close(Uart* u) {
    furi_hal_serial_async_rx_stop(u->handle);
    furi_hal_serial_deinit(u->handle);
    furi_hal_serial_control_release(u->handle);
    expansion_enable(u->expansion);
    furi_record_close(RECORD_EXPANSION);
    furi_stream_buffer_free(u->rx);
    furi_stream_buffer_free(u->log);
    free(u);
}

void uart_send(Uart* u, const char* line) {
    furi_hal_serial_tx(u->handle, (const uint8_t*)line, strlen(line));
    furi_hal_serial_tx(u->handle, (const uint8_t*)"\n", 1);
}

void uart_send_raw(Uart* u, const uint8_t* data, size_t len) {
    furi_hal_serial_tx(u->handle, data, len);
}

void uart_flush(Uart* u) {
    furi_stream_buffer_reset(u->rx);
}

size_t uart_read_log(Uart* u, uint8_t* buf, size_t max) {
    return furi_stream_buffer_receive(u->log, buf, max, 0);
}

int uart_read_line(Uart* u, char* buf, size_t buf_sz, uint32_t timeout_ms) {
    size_t n = 0;
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < timeout_ms) {
        uint8_t b;
        if(furi_stream_buffer_receive(u->rx, &b, 1, 20) == 1) {
            if(b == '\n') {
                buf[n] = 0;
                // strip a trailing CR
                if(n && buf[n - 1] == '\r') buf[--n] = 0;
                return (int)n;
            }
            if(n < buf_sz - 1) buf[n++] = (char)b;
        }
    }
    buf[n] = 0;
    return -1;
}

size_t uart_read_bytes(Uart* u, uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t got = 0;
    uint32_t start = furi_get_tick();
    while(got < len && furi_get_tick() - start < timeout_ms) {
        size_t r = furi_stream_buffer_receive(u->rx, buf + got, len - got, 20);
        if(r) {
            got += r;
            start = furi_get_tick();
        }
    }
    return got;
}
