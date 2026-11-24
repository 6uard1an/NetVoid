// ESP Proxy — Flipper Zero control panel for the ESP32 game-server + web-proxy firmware.
// Talks to the ESP32 over the GPIO UART (pin 13 TX / 14 RX). Protocol: see the firmware's fap.h.
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_box.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include "uart.h"
#include "espflash.h"

#define LOG_CAP 8192 // RAM ring for the live monitor (newest ~8KB kept)
#define LOG_DIR EXT_PATH("apps_data/esp_proxy")
// Firmware bins are bundled in the .fap (fap_file_assets) and deployed to /assets on install.
#define FW_MINI "/assets/mini" // classic ESP32 full firmware (FlipMods Mini)
#define FW_DEVBOARD "/assets/devboard" // ESP32-S2 proxy-only (Wi-Fi Devboard, no SD)

typedef enum {
    ViewMenu,
    ViewList, // submenu: ap settings / wifi / files
    ViewActions, // submenu: per-entry actions
    ViewText,
    ViewInfo, // status
    ViewPopup,
    ViewMonitor, // live serial log
} ViewId;

typedef enum { ModeAp, ModeWifi, ModeFiles, ModeFlash } ListMode;

typedef enum {
    TxApName,
    TxApPass,
    TxWifiPass,
    TxWifiManualName,
    TxWifiManualPass,
    TxNewFolder,
    TxNewFile,
    TxRename,
    TxEdit,
} TxScene;

#define MAX_NETS 32
#define MAX_ENTS 48

typedef struct {
    char apname[64];
    bool apon, hidden, appass, staok, sd;
    char staname[64], ip[24], mac[20], home[140], reset[48];
} Stat;

typedef struct {
    Uart* uart;
    Gui* gui;
    ViewDispatcher* vd;
    Submenu* menu;
    Submenu* list;
    Submenu* actions;
    TextInput* text;
    VariableItemList* info;
    Popup* popup;
    TextBox* mon;

    // serial monitor
    char log[LOG_CAP + 1];
    size_t log_len;
    Storage* storage;
    File* logfile;
    bool logging;
    char logpath[128];
    FuriTimer* timer;
    char flash_text[96]; // persistent: popup_set_text keeps the pointer, not a copy
    bool has_sd; // board reports an SD card (hide file/log options when false)

    ViewId view;
    ListMode mode;
    TxScene tx;
    int popup_ret; // where back from a popup goes (RetMenu/RetAp/RetFiles)
    char text_buf[160];

    // wifi scan
    char nets[MAX_NETS][64];
    bool net_open[MAX_NETS];
    bool net_saved[MAX_NETS];
    int net_count;
    char wifi_ssid[64];

    // files
    char path[192];
    char ents[MAX_ENTS][96];
    bool ent_dir[MAX_ENTS];
    int ent_count;
    char sel[320]; // selected full path
    bool sel_dir;
    char edit_buf[2100];
} App;

static void go(App* a, ViewId v) {
    a->view = v;
    view_dispatcher_switch_to_view(a->vd, v);
}

// Momentum's SDK disables atol(); parse a non-negative integer ourselves.
static long parse_long(const char* s) {
    while(*s == ' ') s++;
    long v = 0;
    while(*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

// ---------------- serial monitor ----------------

// Append captured bytes to the RAM ring; drop a chunk off the front when full.
static void log_append(App* a, const uint8_t* data, size_t n) {
    for(size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if(c == 0) continue; // keep a->log a valid C string for the text box
        if(a->log_len >= LOG_CAP) {
            size_t drop = LOG_CAP / 4;
            memmove(a->log, a->log + drop, a->log_len - drop);
            a->log_len -= drop;
        }
        a->log[a->log_len++] = (char)c;
    }
    a->log[a->log_len] = 0;
}

#define EVT_DRAIN 0xD1A1u
#define EVT_WIFISCAN 0x5CA7u

// Which submenu a popup returns to when dismissed (back). Zero = main menu.
enum { RetMenu, RetAp, RetFiles };
// Fixed submenu ids for the Wi-Fi extras so they never collide with network rows.
#define IDX_WIFI_MANUAL 2000
#define IDX_WIFI_FORGET 2001

static void wifi_populate(App* a); // blocking scan, run async via EVT_WIFISCAN

// Pull everything buffered from the UART capture into the RAM ring (+ SD file).
// Runs on the dispatcher thread (from the timer poke or on entering the monitor).
static void drain_log(App* a) {
    uint8_t tmp[512];
    size_t n;
    bool got = false;
    while((n = uart_read_log(a->uart, tmp, sizeof(tmp))) > 0) {
        log_append(a, tmp, n);
        if(a->logging && a->logfile) storage_file_write(a->logfile, tmp, n);
        got = true;
    }
    if(got && a->view == ViewMonitor) {
        text_box_set_text(a->mon, a->log_len ? a->log : "(waiting for serial...)");
        text_box_set_focus(a->mon, TextBoxFocusEnd);
    }
}

// Background timer fires regardless of the active view; marshal the drain onto
// the dispatcher thread so the log keeps filling while the user is elsewhere.
static void timer_cb(void* ctx) {
    App* a = ctx;
    view_dispatcher_send_custom_event(a->vd, EVT_DRAIN);
}

static bool custom_cb(void* ctx, uint32_t event) {
    App* a = ctx;
    if(event == EVT_DRAIN) {
        drain_log(a);
        return true;
    }
    if(event == EVT_WIFISCAN) {
        wifi_populate(a); // fill the already-shown Wi-Fi list
        return true;
    }
    return false;
}

static bool start_sd_log(App* a) {
    storage_common_mkdir(a->storage, EXT_PATH("apps_data"));
    storage_common_mkdir(a->storage, LOG_DIR);
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        a->logpath,
        sizeof(a->logpath),
        LOG_DIR "/serial_%04u%02u%02u_%02u%02u%02u.txt",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
    if(!a->logfile) a->logfile = storage_file_alloc(a->storage);
    if(!storage_file_open(a->logfile, a->logpath, FSAM_WRITE, FSOM_CREATE_ALWAYS)) return false;
    if(a->log_len) storage_file_write(a->logfile, a->log, a->log_len); // keep history so far
    a->logging = true;
    return true;
}

static void stop_sd_log(App* a) {
    if(a->logfile) storage_file_close(a->logfile);
    a->logging = false;
}

static void scene_monitor(App* a) {
    text_box_set_font(a->mon, TextBoxFontText);
    go(a, ViewMonitor);
    drain_log(a); // pull anything captured while we were elsewhere
    text_box_set_text(a->mon, a->log_len ? a->log : "(waiting for serial...)");
    text_box_set_focus(a->mon, TextBoxFocusEnd);
}

// ---------------- protocol ----------------

static bool proto_ok(App* a, const char* cmd, char* reply, size_t reply_sz) {
    uart_flush(a->uart);
    uart_send(a->uart, cmd);
    char line[256];
    for(int i = 0; i < 30; i++) {
        if(uart_read_line(a->uart, line, sizeof(line), 3000) < 0) break;
        if(strncmp(line, "OK", 2) == 0 || strncmp(line, "ERR", 3) == 0) {
            if(reply) strlcpy(reply, line, reply_sz);
            return strncmp(line, "OK", 2) == 0;
        }
    }
    if(reply) strlcpy(reply, "no response (check wiring/power)", reply_sz);
    return false;
}

// send "pa <path>" to stage a path
static void stage_path(App* a, char letter, const char* path) {
    char cmd[420];
    snprintf(cmd, sizeof(cmd), "p%c %s", letter, path);
    proto_ok(a, cmd, NULL, 0);
}

static bool fetch_stat(App* a, Stat* s) {
    memset(s, 0, sizeof(Stat));
    uart_flush(a->uart);
    uart_send(a->uart, "stat");
    char line[256];
    bool any = false;
    for(int i = 0; i < 24; i++) {
        if(uart_read_line(a->uart, line, sizeof(line), 3000) < 0) break;
        if(strcmp(line, "S.") == 0) {
            any = true;
            break;
        }
        if(strncmp(line, "S ", 2) != 0) continue;
        char* key = line + 2;
        char* val = strchr(key, ' ');
        if(val) *val++ = 0;
        else val = (char*)"";
        if(!strcmp(key, "apname")) strlcpy(s->apname, val, sizeof(s->apname));
        else if(!strcmp(key, "apon")) s->apon = !strcmp(val, "1");
        else if(!strcmp(key, "aphidden")) s->hidden = !strcmp(val, "1");
        else if(!strcmp(key, "appass")) s->appass = !strcmp(val, "1");
        else if(!strcmp(key, "staname")) strlcpy(s->staname, val, sizeof(s->staname));
        else if(!strcmp(key, "staok")) s->staok = !strcmp(val, "1");
        else if(!strcmp(key, "ip")) strlcpy(s->ip, val, sizeof(s->ip));
        else if(!strcmp(key, "mac")) strlcpy(s->mac, val, sizeof(s->mac));
        else if(!strcmp(key, "sd")) s->sd = !strcmp(val, "1");
        else if(!strcmp(key, "home")) strlcpy(s->home, val, sizeof(s->home));
        else if(!strcmp(key, "reset")) strlcpy(s->reset, val, sizeof(s->reset));
    }
    return any;
}

static void popup_msg_ret(App* a, const char* header, const char* text, int ret) {
    a->popup_ret = ret;
    popup_reset(a->popup);
    popup_set_header(a->popup, header, 64, 10, AlignCenter, AlignTop);
    popup_set_text(a->popup, text, 64, 34, AlignCenter, AlignCenter);
    go(a, ViewPopup);
}

static void popup_msg(App* a, const char* header, const char* text) {
    popup_msg_ret(a, header, text, RetMenu);
}

// ---------------- text input ----------------

static void scene_ap(App* a);
static void scene_wifi(App* a);
static void scene_files(App* a);
static void text_done(void* ctx);

static void ask_text(App* a, TxScene tx, const char* header, const char* prefill) {
    a->tx = tx;
    strlcpy(a->text_buf, prefill ? prefill : "", sizeof(a->text_buf));
    text_input_reset(a->text);
    text_input_set_header_text(a->text, header);
    text_input_set_result_callback(a->text, text_done, a, a->text_buf, sizeof(a->text_buf), false);
    go(a, ViewText);
}

static void text_done(void* ctx) {
    App* a = ctx;
    char cmd[320], child[384], reply[160];
    switch(a->tx) {
    case TxApName:
        snprintf(cmd, sizeof(cmd), "apname %s", a->text_buf);
        proto_ok(a, cmd, reply, sizeof(reply));
        popup_msg_ret(a, "AP name", reply, RetAp);
        break;
    case TxApPass:
        snprintf(cmd, sizeof(cmd), "appass %s", a->text_buf);
        proto_ok(a, cmd, reply, sizeof(reply));
        popup_msg_ret(a, "AP password", reply, RetAp);
        break;
    case TxWifiManualName:
        strlcpy(a->wifi_ssid, a->text_buf, sizeof(a->wifi_ssid));
        ask_text(a, TxWifiManualPass, "Password (blank=open)", "");
        break;
    case TxWifiPass:
    case TxWifiManualPass:
        snprintf(cmd, sizeof(cmd), "staname %s", a->wifi_ssid);
        proto_ok(a, cmd, NULL, 0);
        snprintf(cmd, sizeof(cmd), "stapass %s", a->text_buf);
        proto_ok(a, cmd, NULL, 0);
        proto_ok(a, "staconn", reply, sizeof(reply));
        popup_msg(a, a->wifi_ssid, reply[0] == 'O' ? "connecting..." : reply);
        break;
    case TxNewFolder:
        snprintf(child, sizeof(child), "%s%s%s", a->path, a->path[strlen(a->path) - 1] == '/' ? "" : "/", a->text_buf);
        stage_path(a, 'a', child);
        proto_ok(a, "mkdir", reply, sizeof(reply));
        scene_files(a);
        break;
    case TxNewFile:
        snprintf(child, sizeof(child), "%s%s%s", a->path, a->path[strlen(a->path) - 1] == '/' ? "" : "/", a->text_buf);
        stage_path(a, 'a', child);
        proto_ok(a, "put 0", reply, sizeof(reply));
        scene_files(a);
        break;
    case TxEdit: {
        stage_path(a, 'a', a->sel);
        int len = strlen(a->edit_buf);
        snprintf(cmd, sizeof(cmd), "put %d", len);
        uart_flush(a->uart);
        uart_send(a->uart, cmd);
        uart_send_raw(a->uart, (uint8_t*)a->edit_buf, len);
        bool ok = false;
        for(int i = 0; i < 10; i++) {
            char line[64];
            if(uart_read_line(a->uart, line, sizeof(line), 4000) < 0) break;
            if(!strncmp(line, "OK", 2)) {
                ok = true;
                break;
            }
            if(!strncmp(line, "ERR", 3)) break;
        }
        popup_msg_ret(a, "Save", ok ? "saved" : "failed", RetFiles);
        break;
    }
    case TxRename:
        stage_path(a, 'a', a->sel);
        snprintf(child, sizeof(child), "%s%s%s", a->path, a->path[strlen(a->path) - 1] == '/' ? "" : "/", a->text_buf);
        stage_path(a, 'b', child);
        proto_ok(a, "mv", reply, sizeof(reply));
        scene_files(a);
        break;
    }
}

// ---------------- status ----------------

static void scene_status(App* a) {
    Stat s;
    variable_item_list_reset(a->info);
    if(!fetch_stat(a, &s)) {
        VariableItem* it = variable_item_list_add(a->info, "No response", 1, NULL, NULL);
        variable_item_set_current_value_text(it, "check wiring");
        go(a, ViewInfo);
        return;
    }
    VariableItem* it;
    it = variable_item_list_add(a->info, "AP", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.apname);
    it = variable_item_list_add(a->info, "AP running", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.apon ? "yes" : "no");
    it = variable_item_list_add(a->info, "Internet", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.staok ? "connected" : "no");
    it = variable_item_list_add(a->info, "Home Wi-Fi", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.staname[0] ? s.staname : "-");
    it = variable_item_list_add(a->info, "IP", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.ip[0] ? s.ip : "-");
    it = variable_item_list_add(a->info, "SD card", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.sd ? "ok" : "missing");
    it = variable_item_list_add(a->info, "Home page", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.home);
    it = variable_item_list_add(a->info, "Last reset", 1, NULL, NULL);
    variable_item_set_current_value_text(it, s.reset);
    go(a, ViewInfo);
}

// ---------------- AP settings ----------------

enum { ApRename, ApPass, ApHidden, ApStart, ApStop };

static void ap_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    char reply[160];
    Stat s;
    switch(index) {
    case ApRename:
        fetch_stat(a, &s);
        ask_text(a, TxApName, "AP name", s.apname);
        break;
    case ApPass: ask_text(a, TxApPass, "AP password (blank=open)", ""); break;
    case ApHidden:
        fetch_stat(a, &s);
        snprintf(reply, sizeof(reply), "aphidden %d", s.hidden ? 0 : 1);
        proto_ok(a, reply, NULL, 0);
        scene_ap(a); // refresh
        break;
    case ApStart:
        proto_ok(a, "apstart", reply, sizeof(reply));
        scene_monitor(a); // show live serial instead of a success popup
        break;
    case ApStop:
        proto_ok(a, "apstop", reply, sizeof(reply));
        popup_msg_ret(a, "Stop AP", reply[0] ? reply : "done", RetAp);
        break;
    }
}

static void scene_ap(App* a) {
    a->mode = ModeAp;
    Stat s;
    bool ok = fetch_stat(a, &s);
    submenu_reset(a->list);
    submenu_set_header(a->list, "AP settings");
    submenu_add_item(a->list, "Rename AP", ApRename, ap_cb, a);
    submenu_add_item(a->list, "Set password", ApPass, ap_cb, a);
    char h[32];
    snprintf(h, sizeof(h), "Hidden: %s", ok && s.hidden ? "ON" : "off");
    submenu_add_item(a->list, h, ApHidden, ap_cb, a);
    submenu_add_item(a->list, "Start AP", ApStart, ap_cb, a);
    submenu_add_item(a->list, "Stop AP", ApStop, ap_cb, a);
    go(a, ViewList);
}

// ---------------- Home Wi-Fi ----------------

static void wifi_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    if(index == IDX_WIFI_MANUAL) {
        ask_text(a, TxWifiManualName, "Network name", "");
    } else if(index == IDX_WIFI_FORGET) {
        char reply[160];
        proto_ok(a, "staforget", reply, sizeof(reply));
        popup_msg(a, "Home Wi-Fi", "forgotten");
    } else if((int)index < a->net_count) {
        strlcpy(a->wifi_ssid, a->nets[index], sizeof(a->wifi_ssid));
        if(a->net_open[index]) {
            char cmd[128], reply[160];
            snprintf(cmd, sizeof(cmd), "staname %s", a->wifi_ssid);
            proto_ok(a, cmd, NULL, 0);
            proto_ok(a, "stapass ", NULL, 0);
            proto_ok(a, "staconn", reply, sizeof(reply));
            popup_msg(a, a->wifi_ssid, "connecting...");
        } else {
            ask_text(a, TxWifiPass, "Password", "");
        }
    }
}

// Add the two fixed extras (manual + forget) below whatever networks exist.
static void wifi_add_extras(App* a) {
    submenu_add_item(a->list, "+ Manual / hidden", IDX_WIFI_MANUAL, wifi_cb, a);
    submenu_add_item(a->list, "Forget home Wi-Fi", IDX_WIFI_FORGET, wifi_cb, a);
}

// Show the Wi-Fi page immediately, then request the (blocking) scan via an event
// so the list appears instantly and networks fill in a moment later.
static void scene_wifi(App* a) {
    a->mode = ModeWifi;
    a->net_count = 0;
    submenu_reset(a->list);
    submenu_set_header(a->list, "Home Wi-Fi (scanning)");
    wifi_add_extras(a);
    go(a, ViewList);
    view_dispatcher_send_custom_event(a->vd, EVT_WIFISCAN);
}

// Runs from EVT_WIFISCAN: do the scan, then rebuild the list with the results.
static void wifi_populate(App* a) {
    if(a->view != ViewList || a->mode != ModeWifi) return; // user already left
    a->net_count = 0;
    uart_flush(a->uart);
    uart_send(a->uart, "wscan");
    char line[256];
    for(int i = 0; i < 60 && a->net_count < MAX_NETS; i++) {
        if(uart_read_line(a->uart, line, sizeof(line), 6000) < 0) break;
        if(strcmp(line, "N.") == 0) break;
        if(strncmp(line, "N ", 2) != 0) continue;
        // N <rssi> <bars> <open> <saved> <ssid>
        int rssi, bars, open, saved;
        char* p = line + 2;
        if(sscanf(p, "%d %d %d %d ", &rssi, &bars, &open, &saved) != 4) continue;
        int spaces = 0;
        char* q = p;
        while(*q && spaces < 4) {
            if(*q == ' ') spaces++;
            q++;
        }
        strlcpy(a->nets[a->net_count], q, sizeof(a->nets[0]));
        a->net_open[a->net_count] = open;
        a->net_saved[a->net_count] = saved;
        a->net_count++;
    }
    if(a->view != ViewList || a->mode != ModeWifi) return; // left during the scan
    submenu_reset(a->list);
    submenu_set_header(a->list, "Home Wi-Fi");
    for(int i = 0; i < a->net_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s%s %s", a->net_saved[i] ? "* " : "", a->nets[i], a->net_open[i] ? "" : "#");
        submenu_add_item(a->list, label, i, wifi_cb, a);
    }
    wifi_add_extras(a);
}

// ---------------- Files ----------------

enum { AcOpen, AcEdit, AcSetHome, AcRename, AcDelete };

// Fetch a small text file from the card into edit_buf. Returns false if missing/too big.
static bool get_file(App* a, const char* path) {
    stage_path(a, 'a', path);
    uart_flush(a->uart);
    uart_send(a->uart, "get");
    char line[64];
    if(uart_read_line(a->uart, line, sizeof(line), 3000) < 0) return false;
    if(strncmp(line, "G ", 2) != 0) return false;
    long len = parse_long(line + 2);
    if(len < 0 || len > (long)sizeof(a->edit_buf) - 1) return false;
    size_t got = uart_read_bytes(a->uart, (uint8_t*)a->edit_buf, len, 6000);
    a->edit_buf[got] = 0;
    return got == (size_t)len;
}

static void ask_edit(App* a) {
    a->tx = TxEdit;
    text_input_reset(a->text);
    text_input_set_header_text(a->text, "Edit  (OK saves)");
    text_input_set_result_callback(a->text, text_done, a, a->edit_buf, sizeof(a->edit_buf), false);
    go(a, ViewText);
}

static void actions_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    char reply[160];
    switch(index) {
    case AcEdit:
        if(get_file(a, a->sel)) ask_edit(a);
        else popup_msg_ret(a, "Edit", "not text or >2KB", RetFiles);
        break;
    case AcOpen:
        strlcpy(a->path, a->sel, sizeof(a->path));
        scene_files(a);
        break;
    case AcSetHome:
        stage_path(a, 'a', a->sel);
        proto_ok(a, "sethome", reply, sizeof(reply));
        popup_msg_ret(a, "Home page", reply, RetFiles);
        break;
    case AcRename: ask_text(a, TxRename, "New name", ""); break;
    case AcDelete:
        stage_path(a, 'a', a->sel);
        proto_ok(a, "rm", reply, sizeof(reply));
        scene_files(a);
        break;
    }
}

static void file_actions(App* a) {
    submenu_reset(a->actions);
    submenu_set_header(a->actions, a->sel);
    if(a->sel_dir) submenu_add_item(a->actions, "Open", AcOpen, actions_cb, a);
    else {
        submenu_add_item(a->actions, "Edit", AcEdit, actions_cb, a);
        submenu_add_item(a->actions, "Set as home page", AcSetHome, actions_cb, a);
    }
    submenu_add_item(a->actions, "Rename", AcRename, actions_cb, a);
    submenu_add_item(a->actions, "Delete", AcDelete, actions_cb, a);
    go(a, ViewActions);
}

static void files_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    if((int)index < a->ent_count) {
        const char* name = a->ents[index];
        size_t pl = strlen(a->path);
        snprintf(a->sel, sizeof(a->sel), "%s%s%s", a->path, pl && a->path[pl - 1] == '/' ? "" : "/", name);
        a->sel_dir = a->ent_dir[index];
        file_actions(a);
    } else if((int)index == a->ent_count) { // new file
        ask_text(a, TxNewFile, "New file name", "");
    } else { // new folder
        ask_text(a, TxNewFolder, "New folder name", "");
    }
}

static void scene_files(App* a) {
    a->mode = ModeFiles;
    submenu_reset(a->list);
    submenu_set_header(a->list, a->path);
    a->ent_count = 0;
    stage_path(a, 'a', a->path);
    uart_flush(a->uart);
    uart_send(a->uart, "ls");
    char line[256];
    // folders first: collect, then add dirs then files
    for(int i = 0; i < 120 && a->ent_count < MAX_ENTS; i++) {
        if(uart_read_line(a->uart, line, sizeof(line), 5000) < 0) break;
        if(strcmp(line, "E.") == 0) break;
        if(strncmp(line, "ERR", 3) == 0) {
            popup_msg(a, "Files", line);
            return;
        }
        if(strncmp(line, "E ", 2) != 0) continue;
        // E <d|f> <size> <name>
        char type = line[2];
        char* p = line + 4; // skip "E d "
        char* name = strchr(p, ' ');
        if(!name) continue;
        name++;
        strlcpy(a->ents[a->ent_count], name, sizeof(a->ents[0]));
        a->ent_dir[a->ent_count] = (type == 'd');
        a->ent_count++;
    }
    // render: dirs first
    for(int pass = 0; pass < 2; pass++)
        for(int i = 0; i < a->ent_count; i++)
            if(a->ent_dir[i] == (pass == 0)) {
                char label[128];
                snprintf(label, sizeof(label), "%s%s", a->ent_dir[i] ? "/ " : "  ", a->ents[i]);
                submenu_add_item(a->list, label, i, files_cb, a);
            }
    submenu_add_item(a->list, "+ New file", a->ent_count, files_cb, a);
    submenu_add_item(a->list, "+ New folder", a->ent_count + 1, files_cb, a);
    go(a, ViewList);
}

// ---------------- flash ESP32 ----------------

enum { FlMini, FlDevboard, FlHelp };

static void flash_progress(void* ctx, const char* stage, uint32_t cur, uint32_t total, int idx, int count) {
    App* a = ctx;
    if(total) {
        uint32_t pct = cur * 100 / total;
        char bar[11];
        int fill = (int)(pct / 10);
        for(int i = 0; i < 10; i++) bar[i] = i < fill ? '#' : '-';
        bar[10] = 0;
        snprintf(a->flash_text, sizeof(a->flash_text), "%s (%d/%d)\n[%s] %lu%%", stage, idx, count, bar, (unsigned long)pct);
    } else {
        snprintf(a->flash_text, sizeof(a->flash_text), "%s...", stage);
    }
    // a->flash_text persists, so the GUI thread can read it when it redraws.
    popup_set_text(a->popup, a->flash_text, 64, 34, AlignCenter, AlignCenter);
}

// The ESP32 must already be in download mode (hold BOOT, tap RESET) before this runs.
static void do_flash(App* a, const char* dir) {
    popup_reset(a->popup);
    popup_set_header(a->popup, "Flashing ESP32", 64, 8, AlignCenter, AlignTop);
    popup_set_text(a->popup, "connecting...", 64, 36, AlignCenter, AlignCenter);
    a->popup_ret = RetMenu;
    go(a, ViewPopup);
    char err[96] = "";
    bool ok = esp_flash_run(a->uart, a->storage, dir, flash_progress, a, err, sizeof(err));
    strlcpy(a->flash_text, ok ? "Tap RESET to run" : err, sizeof(a->flash_text));
    popup_msg(a, ok ? "Flash complete" : "Flash failed", a->flash_text);
}

static void flash_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    switch(index) {
    case FlMini: do_flash(a, FW_MINI); break;
    case FlDevboard: do_flash(a, FW_DEVBOARD); break;
    case FlHelp:
        popup_msg(
            a,
            "How to flash",
            "Firmware is bundled.\nHold BOOT, tap RESET\non the board, pick it,\nthen wait.");
        break;
    }
}

static void scene_flash(App* a) {
    a->mode = ModeFlash;
    submenu_reset(a->list);
    submenu_set_header(a->list, "Flash - pick board");
    submenu_add_item(a->list, "FlipMods Mini (ESP32)", FlMini, flash_cb, a);
    submenu_add_item(a->list, "WiFi Devboard (ESP32-S2)", FlDevboard, flash_cb, a);
    submenu_add_item(a->list, "How to (read first)", FlHelp, flash_cb, a);
    go(a, ViewList);
}

// ---------------- main menu ----------------

enum { MStart, MStop, MAp, MWifi, MFiles, MStatus, MSerial, MSdlog, MFlash, MAbout };

static void build_menu(App* a);

static void menu_cb(void* ctx, uint32_t index) {
    App* a = ctx;
    switch(index) {
    case MSerial: scene_monitor(a); break;
    case MSdlog:
        if(a->logging) {
            stop_sd_log(a);
            build_menu(a);
            popup_msg(a, "SD logging", "stopped");
        } else if(start_sd_log(a)) {
            build_menu(a);
            popup_msg(a, "Saving to SD", a->logpath);
        } else {
            popup_msg(a, "SD logging", "cannot open file");
        }
        break;
    case MAp: scene_ap(a); break;
    case MWifi: scene_wifi(a); break;
    case MFiles:
        strcpy(a->path, "/");
        scene_files(a);
        break;
    case MStatus: scene_status(a); break;
    case MFlash: scene_flash(a); break;
    case MAbout: popup_msg(a, "NetVoid", "ESP32 server + proxy\ncontrol over UART"); break;
    }
}

// Quick one-shot "stat" to learn if the connected board has an SD card (bounded ~1.5s).
// Defaults to true (show everything) if the board doesn't answer.
static bool probe_sd(App* a) {
    uart_flush(a->uart);
    uart_send(a->uart, "stat");
    char line[256];
    uint32_t t0 = furi_get_tick();
    while(furi_get_tick() - t0 < 1500) {
        if(uart_read_line(a->uart, line, sizeof(line), 300) < 0) continue;
        if(strcmp(line, "S.") == 0) break;
        if(strncmp(line, "S sd ", 5) == 0) return line[5] == '1';
    }
    return true;
}

static void build_menu(App* a) {
    submenu_reset(a->menu);
    submenu_set_header(a->menu, "NetVoid");
    submenu_add_item(a->menu, "AP settings", MAp, menu_cb, a);
    submenu_add_item(a->menu, "Home Wi-Fi", MWifi, menu_cb, a);
    if(a->has_sd) submenu_add_item(a->menu, "Files", MFiles, menu_cb, a); // SD only
    submenu_add_item(a->menu, "Status", MStatus, menu_cb, a);
    submenu_add_item(a->menu, "Serial monitor", MSerial, menu_cb, a);
    if(a->has_sd) // SD logging writes to the Flipper SD, but it's an SD-board feature
        submenu_add_item(a->menu, a->logging ? "SD log: ON (stop)" : "SD log: OFF (start)", MSdlog, menu_cb, a);
    submenu_add_item(a->menu, "Flash ESP32", MFlash, menu_cb, a);
    submenu_add_item(a->menu, "About", MAbout, menu_cb, a);
}

// ---------------- navigation (back) ----------------

static bool back_cb(void* ctx) {
    App* a = ctx;
    switch(a->view) {
    case ViewMenu:
        return false; // exit app
    case ViewActions:
        scene_files(a);
        return true;
    case ViewList:
        if(a->mode == ModeFiles && strcmp(a->path, "/") != 0) {
            char* slash = strrchr(a->path, '/');
            if(slash && slash != a->path) *slash = 0;
            else strcpy(a->path, "/");
            scene_files(a);
        } else {
            go(a, ViewMenu);
        }
        return true;
    default: // ViewText, ViewInfo, ViewPopup, ViewMonitor
        if(a->view == ViewPopup && a->popup_ret == RetAp) {
            scene_ap(a);
        } else if(a->view == ViewPopup && a->popup_ret == RetFiles) {
            scene_files(a);
        } else {
            go(a, ViewMenu);
        }
        return true;
    }
}

// ---------------- lifecycle ----------------

static App* app_alloc(void) {
    App* a = malloc(sizeof(App));
    memset(a, 0, sizeof(App));
    a->uart = uart_open();
    a->gui = furi_record_open(RECORD_GUI);
    a->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(a->vd, a->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(a->vd, a);
    view_dispatcher_set_navigation_event_callback(a->vd, back_cb);
    view_dispatcher_set_custom_event_callback(a->vd, custom_cb);

    a->storage = furi_record_open(RECORD_STORAGE);

    a->menu = submenu_alloc();
    a->list = submenu_alloc();
    a->actions = submenu_alloc();
    a->text = text_input_alloc();
    a->info = variable_item_list_alloc();
    a->popup = popup_alloc();
    a->mon = text_box_alloc();

    view_dispatcher_add_view(a->vd, ViewMenu, submenu_get_view(a->menu));
    view_dispatcher_add_view(a->vd, ViewList, submenu_get_view(a->list));
    view_dispatcher_add_view(a->vd, ViewActions, submenu_get_view(a->actions));
    view_dispatcher_add_view(a->vd, ViewText, text_input_get_view(a->text));
    view_dispatcher_add_view(a->vd, ViewInfo, variable_item_list_get_view(a->info));
    view_dispatcher_add_view(a->vd, ViewPopup, popup_get_view(a->popup));
    view_dispatcher_add_view(a->vd, ViewMonitor, text_box_get_view(a->mon));

    a->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, a);
    furi_timer_start(a->timer, furi_ms_to_ticks(150));

    a->has_sd = probe_sd(a); // hide SD-only menu items on a board without a card
    build_menu(a);
    return a;
}

static void app_free(App* a) {
    furi_timer_stop(a->timer);
    furi_timer_free(a->timer);
    for(int v = ViewMenu; v <= ViewMonitor; v++) view_dispatcher_remove_view(a->vd, v);
    submenu_free(a->menu);
    submenu_free(a->list);
    submenu_free(a->actions);
    text_input_free(a->text);
    variable_item_list_free(a->info);
    popup_free(a->popup);
    text_box_free(a->mon);
    if(a->logfile) {
        if(a->logging) storage_file_close(a->logfile);
        storage_file_free(a->logfile);
    }
    furi_record_close(RECORD_STORAGE);
    view_dispatcher_free(a->vd);
    furi_record_close(RECORD_GUI);
    uart_close(a->uart);
    free(a);
}

int32_t esp_proxy_app(void* p) {
    UNUSED(p);
    App* a = app_alloc();
    go(a, ViewMenu);
    view_dispatcher_run(a->vd);
    app_free(a);
    return 0;
}
