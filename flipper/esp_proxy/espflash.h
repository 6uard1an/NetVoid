// Flash the ESP32 over the GPIO UART using its ROM serial bootloader (the public esptool protocol).
// The ESP32 must already be in download mode (hold BOOT, tap RESET). Images are read from the
// Flipper's SD card and listed in <dir>/flash.txt as "<hex offset> <filename>" lines.
#pragma once
#include <furi.h>
#include <storage/storage.h>
#include "uart.h"

// Progress UI hook: a stage label (filename), current/total blocks in this file (total 0 =
// indeterminate), and which file this is of how many (idx/count, both 0 before flashing starts).
typedef void (*EspFlashProgress)(
    void* ctx,
    const char* stage,
    uint32_t cur,
    uint32_t total,
    int idx,
    int count);

// Flash every image in <dir>/flash.txt. Returns true on success; on failure fills err.
bool esp_flash_run(
    Uart* uart,
    Storage* storage,
    const char* dir,
    EspFlashProgress cb,
    void* ctx,
    char* err,
    size_t err_sz);
