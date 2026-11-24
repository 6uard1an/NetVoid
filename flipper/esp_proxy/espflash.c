// ESP32 ROM-bootloader flasher over the Flipper GPIO UART. Self-contained implementation of the
// public esptool serial protocol (SLIP framing + SYNC / SPI_ATTACH / FLASH_BEGIN/DATA/END), in
// ROM mode (no stub, uncompressed). Verified on the Sacred Labs FlipMods Mini.
#include "espflash.h"
#include <stdlib.h>
#include <string.h>

#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define CMD_FLASH_BEGIN 0x02
#define CMD_FLASH_DATA 0x03
#define CMD_FLASH_END 0x04
#define CMD_SYNC 0x08
#define CMD_SPI_ATTACH 0x0D

#define BLOCK 4096u // flash data block size
#define CK_SEED 0xEFu // FLASH_DATA checksum seed

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void slip_put(uint8_t* o, uint32_t* n, uint8_t b) {
    if(b == SLIP_END) {
        o[(*n)++] = SLIP_ESC;
        o[(*n)++] = SLIP_ESC_END;
    } else if(b == SLIP_ESC) {
        o[(*n)++] = SLIP_ESC;
        o[(*n)++] = SLIP_ESC_ESC;
    } else {
        o[(*n)++] = b;
    }
}

// Build and send one SLIP-framed command. checksum is only meaningful for FLASH_DATA.
static void send_cmd(Uart* u, uint8_t cmd, const uint8_t* data, uint16_t len, uint32_t checksum) {
    uint8_t hdr[8] = {
        0x00,
        cmd,
        (uint8_t)len,
        (uint8_t)(len >> 8),
        (uint8_t)checksum,
        (uint8_t)(checksum >> 8),
        (uint8_t)(checksum >> 16),
        (uint8_t)(checksum >> 24)};
    uint8_t* out = malloc((size_t)2 * (8 + len) + 2);
    if(!out) return;
    uint32_t n = 0;
    out[n++] = SLIP_END;
    for(int i = 0; i < 8; i++) slip_put(out, &n, hdr[i]);
    for(uint16_t i = 0; i < len; i++) slip_put(out, &n, data[i]);
    out[n++] = SLIP_END;
    uart_send_raw(u, out, n);
    free(out);
}

// Read one SLIP response frame. Fills cmd/value and (optionally) the data bytes. Returns false on
// timeout. The ROM reply layout is: 0x01, cmd, size(2), value(4), data..., with the last two data
// bytes being [status, error].
static bool read_frame(Uart* u, uint8_t* cmd, uint8_t* data, uint16_t* dlen, uint32_t* value, uint32_t timeout_ms) {
    int c;
    do {
        c = uart_read_byte(u, timeout_ms);
        if(c < 0) return false;
    } while(c != SLIP_END);

    uint8_t buf[64];
    uint16_t n = 0;
    while(true) {
        c = uart_read_byte(u, timeout_ms);
        if(c < 0) return false;
        if(c == SLIP_END) {
            if(n == 0) continue; // skip empty frame between back-to-back ENDs
            break;
        }
        if(c == SLIP_ESC) {
            c = uart_read_byte(u, timeout_ms);
            if(c < 0) return false;
            c = (c == SLIP_ESC_END) ? SLIP_END : (c == SLIP_ESC_ESC) ? SLIP_ESC : c;
        }
        if(n < sizeof(buf)) buf[n++] = (uint8_t)c;
    }
    if(n < 8 || buf[0] != 0x01) return false;
    if(cmd) *cmd = buf[1];
    uint16_t size = (uint16_t)(buf[2] | (buf[3] << 8));
    if(value) *value = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint16_t dl = (uint16_t)(n - 8);
    if(dl > size) dl = size;
    if(data && dlen) {
        memcpy(data, buf + 8, dl);
        *dlen = dl;
    }
    return true;
}

// Send a command and wait for its matching reply with status == OK.
static bool command(Uart* u, uint8_t cmd, const uint8_t* data, uint16_t len, uint32_t checksum, uint32_t timeout_ms) {
    send_cmd(u, cmd, data, len, checksum);
    uint32_t t0 = furi_get_tick();
    while(furi_get_tick() - t0 < timeout_ms) {
        uint8_t rc;
        uint8_t rd[64];
        uint16_t rl = 0;
        if(!read_frame(u, &rc, rd, &rl, NULL, timeout_ms)) return false;
        if(rc != cmd) continue; // stray/boot message, keep looking
        if(rl >= 2 && rd[rl - 2] != 0) return false; // status byte non-zero = failed
        return true;
    }
    return false;
}

static bool sync_rom(Uart* u) {
    uint8_t d[36];
    d[0] = 0x07;
    d[1] = 0x07;
    d[2] = 0x12;
    d[3] = 0x20;
    memset(d + 4, 0x55, 32);
    for(int tries = 0; tries < 12; tries++) {
        send_cmd(u, CMD_SYNC, d, sizeof(d), 0);
        uint8_t rc;
        uint8_t rd[64];
        uint16_t rl;
        uint32_t t0 = furi_get_tick();
        bool got = false;
        while(furi_get_tick() - t0 < 350) {
            if(read_frame(u, &rc, rd, &rl, NULL, 350) && rc == CMD_SYNC) {
                got = true;
                break;
            }
        }
        if(got) {
            // drain the extra SYNC replies the ROM emits
            uint8_t rc2;
            uint16_t rl2;
            for(int i = 0; i < 8; i++)
                if(!read_frame(u, &rc2, NULL, &rl2, NULL, 40)) break;
            return true;
        }
    }
    return false;
}

static bool flash_file(
    Uart* u,
    Storage* st,
    const char* path,
    uint32_t offset,
    const char* label,
    uint8_t* block,
    uint8_t* pkt,
    EspFlashProgress cb,
    void* ctx,
    char* err,
    size_t err_sz) {
    File* f = storage_file_alloc(st);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        snprintf(err, err_sz, "missing %s", label);
        storage_file_free(f);
        return false;
    }
    uint32_t size = (uint32_t)storage_file_size(f);
    uint32_t nblocks = (size + BLOCK - 1) / BLOCK;
    if(nblocks == 0) nblocks = 1;

    uint8_t fb[16];
    wr32(fb, size);
    wr32(fb + 4, nblocks);
    wr32(fb + 8, BLOCK);
    wr32(fb + 12, offset);
    if(!command(u, CMD_FLASH_BEGIN, fb, sizeof(fb), 0, 30000)) {
        snprintf(err, err_sz, "erase failed: %s", label);
        storage_file_close(f);
        storage_file_free(f);
        return false;
    }

    bool ok = true;
    for(uint32_t seq = 0; seq < nblocks; seq++) {
        uint16_t got = storage_file_read(f, block, BLOCK);
        if(got < BLOCK) memset(block + got, 0xFF, BLOCK - got);
        wr32(pkt, BLOCK);
        wr32(pkt + 4, seq);
        wr32(pkt + 8, 0);
        wr32(pkt + 12, 0);
        memcpy(pkt + 16, block, BLOCK);
        uint32_t chk = CK_SEED;
        for(uint32_t i = 0; i < BLOCK; i++) chk ^= block[i];
        bool wrote = false;
        for(int retry = 0; retry < 3 && !wrote; retry++)
            wrote = command(u, CMD_FLASH_DATA, pkt, 16 + BLOCK, chk, 3000);
        if(!wrote) {
            snprintf(err, err_sz, "write failed: %s", label);
            ok = false;
            break;
        }
        if((seq & 3) == 0 || seq == nblocks - 1) cb(ctx, label, seq + 1, nblocks);
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

bool esp_flash_run(
    Uart* uart,
    Storage* storage,
    const char* dir,
    EspFlashProgress cb,
    void* ctx,
    char* err,
    size_t err_sz) {
    char path[300];
    snprintf(path, sizeof(path), "%s/flash.txt", dir);
    File* mf = storage_file_alloc(storage);
    if(!storage_file_open(mf, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        snprintf(err, err_sz, "no flash.txt on SD");
        storage_file_free(mf);
        return false;
    }
    char man[512];
    uint16_t mn = storage_file_read(mf, man, sizeof(man) - 1);
    man[mn] = 0;
    storage_file_close(mf);
    storage_file_free(mf);

    cb(ctx, "Connecting", 0, 0);
    uart_flush(uart);
    if(!sync_rom(uart)) {
        snprintf(err, err_sz, "No bootloader. Hold BOOT, tap RESET.");
        return false;
    }
    uint8_t att[8] = {0};
    command(uart, CMD_SPI_ATTACH, att, sizeof(att), 0, 3000); // best-effort; ESP32 default SPI

    uint8_t* block = malloc(BLOCK);
    uint8_t* pkt = malloc(16 + BLOCK);
    if(!block || !pkt) {
        free(block);
        free(pkt);
        snprintf(err, err_sz, "out of memory");
        return false;
    }

    bool ok = true;
    char* p = man;
    while(*p && ok) {
        while(*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if(!*p) break;
        if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint32_t off = 0;
        int hv;
        while((hv = hexval(*p)) >= 0) {
            off = off * 16 + (uint32_t)hv;
            p++;
        }
        while(*p == ' ' || *p == '\t') p++;
        char fname[128];
        int fi = 0;
        while(*p && *p != '\r' && *p != '\n' && fi < (int)sizeof(fname) - 1) fname[fi++] = *p++;
        fname[fi] = 0;
        if(fi > 0) {
            char fpath[300];
            snprintf(fpath, sizeof(fpath), "%s/%s", dir, fname);
            ok = flash_file(uart, storage, fpath, off, fname, block, pkt, cb, ctx, err, err_sz);
        }
        while(*p && *p != '\n') p++;
    }

    free(block);
    free(pkt);

    if(ok) {
        uint8_t fe[4] = {0, 0, 0, 0}; // FLASH_END, flag 0 = reboot into the new app
        command(uart, CMD_FLASH_END, fe, sizeof(fe), 0, 3000);
    }
    return ok;
}
