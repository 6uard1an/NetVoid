// Flash the ESP32 over the GPIO UART using its ROM serial bootloader (the public esptool protocol).
// The ESP32 must already be in download mode (hold BOOT, tap RESET). Images are read from the
// Flipper's SD card and listed in <dir>/flash.txt as "<hex offset> <filename>" lines.
#pragma once
#include <furi.h>
#include <storage/storage.h>
#include "uart.h"

// Progress UI hook: a short stage label plus current/total blocks (total 0 = indeterminate).
typedef void (*EspFlashProgress)(void* ctx, const char* stage, uint32_t cur, uint32_t total);

// Flash every image in <dir>/flash.txt. Returns true on success; on failure fills err.
bool esp_flash_run(
    Uart* uart,
    Storage* storage,
    const char* dir,
    EspFlashProgress cb,
    void* ctx,
    char* err,
    size_t err_sz);
