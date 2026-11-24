# NetVoid — ESP32 Web Proxy + Game Server, and its Flipper Zero Control App

Complete technical documentation for the two programs in this repository:

1. **The ESP32 firmware** (`esp32-proxy/`) — a self-contained Wi-Fi access point that (a) serves
   games/apps off an SD card and (b) acts as a full HTTPS→HTTP web proxy, all on a bare ESP32
   with no PSRAM.
2. **NetVoid** (`flipper/esp_proxy/`) — a Flipper Zero application that drives the firmware over a
   GPIO UART: start/stop the AP, rename it, pick a home Wi-Fi, browse/edit the SD card, watch a live
   serial monitor, and log serial to the Flipper's SD card.

The two talk to each other over a single 3-wire UART at **115200 baud, 8N1**, using a line-based
protocol that is backward compatible with the original Flipper-extension sketch.

---

## Table of contents

- [1. System overview](#1-system-overview)
- [2. Hardware and wiring](#2-hardware-and-wiring)
- [3. Repository layout](#3-repository-layout)
- [4. The ESP32 firmware](#4-the-esp32-firmware)
  - [4.1 Build and flash](#41-build-and-flash)
  - [4.2 Boot sequence and persistence](#42-boot-sequence-and-persistence)
  - [4.3 Networking model (AP + STA, DNS, mDNS)](#43-networking-model-ap--sta-dns-mdns)
  - [4.4 The web proxy](#44-the-web-proxy)
  - [4.5 The SD card game/app server](#45-the-sd-card-gameapp-server)
  - [4.6 Custom MAC address](#46-custom-mac-address)
  - [4.7 Crash recovery and brownouts](#47-crash-recovery-and-brownouts)
  - [4.8 Memory tuning (lowmem.cpp)](#48-memory-tuning-lowmemcpp)
  - [4.9 Serial console — human commands](#49-serial-console--human-commands)
  - [4.10 Serial console — machine protocol (fap.h)](#410-serial-console--machine-protocol-faph)
- [5. NetVoid — the Flipper app](#5-netvoid--the-flipper-app)
  - [5.1 Build and install](#51-build-and-install)
  - [5.2 Architecture](#52-architecture)
  - [5.3 The UART layer](#53-the-uart-layer)
  - [5.4 Protocol client helpers](#54-protocol-client-helpers)
  - [5.5 Screens](#55-screens)
  - [5.6 Navigation model](#56-navigation-model)
  - [5.7 Serial monitor and SD logging](#57-serial-monitor-and-sd-logging)
  - [5.8 Name and icon](#58-name-and-icon)
- [6. End-to-end flows](#6-end-to-end-flows)
- [7. Troubleshooting](#7-troubleshooting)
- [8. Limits and future work](#8-limits-and-future-work)

---

## 1. System overview

```
        ┌─────────────────┐   GPIO UART 115200 8N1   ┌──────────────────────┐
        │  Flipper Zero   │  pin13 TX ─────► ESP32 RX │                      │
        │   "NetVoid"     │  pin14 RX ◄───── ESP32 TX │   ESP32 firmware     │
        │    (FAP app)    │  GND ──────────────── GND │  (AP + proxy + SD)   │
        └─────────────────┘                           └──────────┬───────────┘
                                                                 │ Wi-Fi
                           ┌─────────────────────────────────────┼───────────────┐
                           │                                     (AP)            (STA)
                    ┌──────▼──────┐                        ┌──────▼──────┐  ┌──────▼──────┐
                    │ Your phone/ │  joins ESP32's open AP │  ESP32 soft │  │ Home router │
                    │  laptop     │ ◄───── HTTP ────────── │     AP      │  │  (internet) │
                    └─────────────┘                        └─────────────┘  └─────────────┘
```

- Clients join the ESP32's **open access point** and browse normally over plain HTTP.
- For SD content (games/apps) the ESP32 serves files directly.
- For the open internet the ESP32's **proxy** makes the real HTTPS request itself (it owns the TLS),
  strips all content-restriction headers, and streams the result back to the client over HTTP.
- The Flipper is an optional control surface. The firmware is fully usable without it (serial console
  or the on-SD web UI); NetVoid just makes the common operations one-button.

---

## 2. Hardware and wiring

| Signal | ESP32 | Flipper Zero | Notes |
|---|---|---|---|
| UART TX (Flipper→ESP32) | RX (GPIO3 / USART RX) | **pin 13** (TX) | 115200 8N1 |
| UART RX (ESP32→Flipper) | TX (GPIO1 / USART TX) | **pin 14** (RX) | |
| Ground | GND | **GND** (e.g. pin 18) | common ground required |
| SD card CS | **GPIO5** | — | SPI, card lives in the ESP32 the whole time |
| BOOT button | GPIO0 | — | hold 5 s to factory-reset |

- The **SD card stays in the ESP32**, never the PC. It is mounted at power-on (`sdMount()` in
  `sdsite.h`), which steps down SPI clock through `{8,4,2,1,0.4} MHz` and verifies it can actually
  read a directory before accepting the card.
- To **flash firmware** the ESP32 must be on the PC's USB directly (it enumerates as a CH340, e.g.
  `COM8`). Pull it out of the Flipper for flashing, then put it back.
- To **install the Flipper app** the Flipper is on USB (enumerates as "USB Serial Device", e.g.
  `COM9`). Only one of the two is on USB at a time.

---

## 3. Repository layout

```
esp32-proxy/
├── build.py                 Custom compiler/flasher (no Arduino IDE / arduino-cli)
├── connect-wifi.ps1         Reads the PC's saved Wi-Fi password via netsh and sends setwifi= over serial
├── README.md                Short proxy-focused readme
├── DOCUMENTATION.md         This file
│
├── esp32-proxy/             ── ESP32 firmware (Arduino-ESP32 core 3.3.7 / ESP-IDF 5.5) ──
│   ├── esp32-proxy.ino      Arduino sketch entry (thin; real code is in proxy.cpp)
│   ├── proxy.cpp            Main: Wi-Fi, HTTP server, proxy engine, serial console, setup()/loop()
│   ├── sdsite.h             SD mount + file server + /api/* + setMac()
│   ├── fap.h                Machine protocol spoken to the Flipper app (fapCmd())
│   ├── rewrite.h            Streaming HTML/CSS/JS URL rewriter (Rewriter class)
│   ├── gunzip.h             Streaming gzip inflater that runs in instruction RAM
│   ├── hook_gz.h            Gzipped runtime hook.js injected into proxied HTML
│   ├── pages.h              Built-in HTML (proxy address bar etc.)
│   └── lowmem.cpp           Linker --wrap shims that shrink TLS/Wi-Fi buffers
│
├── flipper/esp_proxy/       ── NetVoid Flipper Zero app (ufbt, Momentum SDK, API 87.1) ──
│   ├── application.fam      App manifest (appid, name "NetVoid", category GPIO, fap_icon)
│   ├── esp_proxy.c          Whole app: ViewDispatcher, all screens, protocol client
│   ├── uart.c / uart.h      GPIO USART link + the serial-monitor capture buffer
│   └── icon.png             10×10 1-bit app icon
│
└── sd-browser/index.html    The "Browser" app page written onto the SD card (address bar + Wi-Fi/MAC UI)
```

> Note: `esptool`, `ufbt` build output, and `backup/` are git-ignored.

---

## 4. The ESP32 firmware

### 4.1 Build and flash

The project does **not** use the Arduino IDE. `build.py` invokes the xtensa compiler directly and links
in `lowmem.cpp` with a set of `-Wl,--wrap=` flags that patch the prebuilt core libraries.

```powershell
python build.py            # compile only -> %TEMP%\esp32-proxy-build\proxy.elf (+ prints image size)
python build.py COM8       # compile + flash to the ESP32 on COM8 (esptool, writes app at 0x10000)
python build.py --sd-test  # build with LittleFS standing in for the SD card (SD_TEST define)
```

- Warnings are errors (`-Werror`): `format-truncation`, `int-conversion` and friends fail the build.
- Flashing takes ~70 s for the ~1.25 MB image and verifies the hash.
- Restore the stock firmware with `esptool --port COM8 write-flash 0 backup/original-flash.bin`.

### 4.2 Boot sequence and persistence

`setup()` (in `proxy.cpp`):

1. `Serial.begin(115200)`, pull up GPIO0 (BOOT), open NVS namespace `pxproxy`.
2. Print **`Waiting for AP name via serial...`** — the original Flipper handshake. On a clean power-on
   or a serial `reset`, the AP does **not** come up until a `setap=Name;password` (or NetVoid's
   `apstart`) arrives. This preserves the original extension behaviour exactly.
3. `WiFi.mode(WIFI_AP_STA)`, load saved values, disable modem sleep.
4. Apply a saved custom STA MAC if present.
5. If a home Wi-Fi is saved, begin connecting (auto-reconnect on).
6. Allocate the 32 KB gzip window in executable RAM, mount the SD card.
7. Create worker tasks, bind the HTTP listener on port 80, set TX power.
8. **Crash recovery:** if the reset reason is a panic/watchdog/brownout *and* the AP was up before
   (`apup` flag), bring the AP straight back up without waiting for `setap`.

**NVS keys (namespace `pxproxy`):**

| Key | Meaning |
|---|---|
| `ap` / `appw` | AP SSID / password |
| `apup` | AP was running (used for crash recovery) |
| `aphidden` | AP SSID hidden (not broadcast) |
| `sta` / `stapw` | Home Wi-Fi SSID / password |
| `home` | Default home page path (`/index.html`) |
| `mac` | 6-byte custom STA MAC (absent = factory MAC) |

Holding **BOOT for 5 s** clears all settings. `factory` over serial does the same.

### 4.3 Networking model (AP + STA, DNS, mDNS)

- Runs AP and STA simultaneously (`WIFI_AP_STA`). AP IP is `192.168.4.1`.
- The firmware answers **DNS for every name** (captive style) so any hostname resolves to the ESP32.
  This is what lets `http://example.com.esp32/` route back to the board.
- mDNS/`*.local` is served so `http://<ap-name>.local/` and `http://proxy.esp32/` work.
- TX power is capped at `TX_POWER = 44` quarter-dBm (**11 dBm**); at full power this board corrupts
  its own reception. Wi-Fi power save is disabled for latency.

### 4.4 The web proxy

Flow for a proxied request (`proxy.cpp`, worker tasks):

1. Client requests `http://example.com.esp32/path` (DNS already pointed it at the ESP32).
2. The worker opens an upstream TLS 1.2 connection to the real `example.com` (`esp_tls` +
   certificate bundle). RSA certs are requested first because the chip verifies them in hardware
   (~0.5 s vs ~1.8 s). Connections are pooled/reused and TLS sessions resumed.
3. The response streams back through `Rewriter` (`rewrite.h`), which rewrites absolute/relative URLs
   on the fly (`https://host/…` → `http://host.esp32/…`) across HTML, CSS and JS without buffering
   the whole body.
4. **All content restrictions are stripped:** CSP (header *and* `<meta>`), X-Frame-Options,
   COOP/COEP/CORP, Permissions-Policy, Referrer-Policy, HSTS, `nosniff`, SRI. CORS is opened to `*`.
5. Gzipped responses are inflated **on the board** using the 32 KB instruction-RAM window
   (`gunzip.h`) so pages can be rewritten; scripts/styles/images/fonts often pass through still
   compressed.
6. A small runtime hook (`hook_gz.h` → `hook.js`) is injected into HTML to rewrite URLs created at
   runtime by JavaScript.
7. Static assets are marked cacheable (1–7 days) and served from cache on reload; force-reload
   (Ctrl/Cmd+F5) to refetch. Images load lazily as you scroll.
8. Ads/telemetry hosts are short-circuited locally (`isTracker` in `proxy.cpp`).
9. Private/LAN addresses are blocked (`ALLOW_PRIVATE`).

Constraints: ≤2 concurrent HTTPS upstreams (no PSRAM), TLS 1.2 only, no HTTP/2, no service workers,
no WebCrypto subtle/camera/DRM. Bot checks (Cloudflare etc.) may reject the proxy.

### 4.5 The SD card game/app server

`sdsite.h` serves the card (mounted at VFS root, CS pin 5):

- `GET /` → the configured **home page** (`homePage`, default `/index.html`).
- File and directory listings; `GET /list?dir=<path>` for JSON listings.
- `/edit` endpoint: **POST** uploads a file, **PUT** creates, **DELETE** removes. Guarded so that a
  website opened *through the proxy* cannot call `/edit` (origin check) — only pages served from the
  card may change the card.
- `/api/*` endpoints back the on-SD "Browser" UI (`sd-browser/index.html`): Wi-Fi scan/connect/forget
  and MAC get/set, returning JSON.
- The menu's **Browser** app opens the proxy address bar (`http://proxy.esp32/`).

### 4.6 Custom MAC address

`setMac(spec, outMac)` in `sdsite.h` sets the **station** MAC so the ESP32 presents itself as a
specific device on your home network. Accepted `spec`:

- empty / `default` → restore the factory STA MAC (removes the `mac` NVS key).
- `random` → a random locally-administered unicast MAC.
- `AA:BB:CC:DD:EE:FF` (or with `-`) → that exact MAC. First octet must be even (unicast).

**Soft-AP client collision handling (added):** the Wi-Fi stack refuses a STA MAC that is currently in
use by a device associated to the ESP32's own soft-AP. Before applying a custom MAC the firmware now
calls `esp_wifi_deauth_sta(0)` to **kick all AP clients** (freeing the MAC if one of them held it),
waits briefly, applies the MAC, and **retries once** (kick-all + longer wait) if the first attempt
still fails. Kicked clients reconnect on their own; the ESP32's STA connection re-begins right after
the MAC is applied. Set via serial `mac=…`, the `/api/mac` endpoint, or NetVoid's `setmac`.

### 4.7 Crash recovery and brownouts

A heavy page can reset the board — usually a **brownout** on weak USB power, sometimes **out of
memory**. Mitigations:

- The AP restarts itself after a crash (see boot step 8), so you reconnect without re-sending `setap`.
- `status` reports `last reset:` — `brownout` → use a stronger charger/cable; `crash (out of memory?)`
  → the page was too heavy.
- A memory guardian in `loop()` closes idle pooled upstream connections when the largest free block
  drops below a threshold.
- `crashtest` forces a panic to verify recovery.

### 4.8 Memory tuning (lowmem.cpp)

`lowmem.cpp` is linked with `-Wl,--wrap=` shims (configured in `build.py`) that patch the prebuilt
libraries without editing them:

- `mbedtls_calloc` → shrinks the TLS output buffer to 4 KB.
- `esp_wifi_init` → reduces dynamic RX buffers.
- TLS session-resumption hooks and TCP allocation tuning.

Because of this you **must build with `build.py`**, not the Arduino IDE (which wouldn't apply the
linker wraps).

### 4.9 Serial console — human commands

Line-based, 115200 baud. `serialCmd()` first offers the line to `fapCmd()` (the machine protocol,
§4.10); if that doesn't claim it, these human commands apply. Backward compatible with the original
sketch.

| Command | Effect |
|---|---|
| `setap=Name;password` | Start the AP (original handshake). Ignored if already started. Empty password = open. |
| `setwifi=Name;password` | Save home Wi-Fi, then **restart**. |
| `status` (or any unmatched line) | Print full status (AP, STA, IP, SD, last reset…). |
| `scan` | Report how crowded each 2.4 GHz channel is (count + strongest dBm). |
| `mac` | Print current STA MAC and whether it's custom. |
| `mac=AA:BB:…` / `mac=random` / `mac=default` | Set/clear the custom STA MAC (§4.6). |
| `reset` / `restart` | `Resetting...` then reboot. |
| `factory` | Clear all settings, then restart. |
| `selftest` | Run internal self-test. |
| `psram` | Report PSRAM presence/size. |
| `bench` | Benchmark the HTML rewriter (KB/s). |
| `tx=<n>` | Set max TX power (quarter-dBm) at runtime. |
| `lan` | **Testing only:** open the proxy to your home LAN until restart. |
| `crashtest` | Force a panic to test auto-recovery. |

### 4.10 Serial console — machine protocol (fap.h)

Spoken by NetVoid. One command per line. Values that can contain spaces (SSIDs, paths) are **staged**
by their own command first. Replies are `OK`, `OK <x>`, `ERR <msg>`, or a block terminated by a lone
`.` line. Binary file transfers carry an explicit byte length.

| Command | Reply | Purpose |
|---|---|---|
| `F?` | `F! esp32-proxy 1` | Handshake + protocol version |
| `stat` | `S <key> <val>` lines, then `S.` | Full status snapshot (apname, apon, aphidden, staname, staok, ip, rssi, mac, custommac, sd, home, reset) |
| `wscan` | `N <rssi> <bars> <open> <saved> <ssid>` lines, then `N.` | Scan nearby networks |
| `pa <path>` / `pb <path>` | `OK` | Stage path A / path B (for file ops) |
| `staname <ssid>` / `stapass <pass>` | `OK` | Stage home Wi-Fi credentials |
| `apname <name>` | `OK` / `ERR` | Rename AP (persists + re-applies live) |
| `appass <pass>` | `OK` / `ERR` | Set AP password (empty or 8–63 chars) |
| `aphidden <0\|1>` | `OK` | Hide/show the AP SSID |
| `apstart` | `OK` | Start the AP (emits `OK` after the human "AP started" lines) |
| `apstop` | `OK` | Stop the AP, clear `apup` |
| `staconn` | `OK` / `ERR` | Connect to the staged home Wi-Fi |
| `staforget` | `OK` | Forget home Wi-Fi |
| `setmac <spec>` | `OK <mac>` / `ERR` | Set custom STA MAC (§4.6) |
| `ls [path]` | `E <d\|f> <size> <name>` lines, then `E.` | List a folder (staged path A if omitted) |
| `mkdir` | `OK` / `ERR` | Create folder at staged path A |
| `rm` | `OK` / `ERR` | Delete staged path A (not root/non-existent) |
| `mv` | `OK` / `ERR` | Rename/move staged path A → path B |
| `sethome` | `OK` / `ERR` | Set staged path A as the home page |
| `get` | `G <len>\n<bytes>` | Read staged path A (≤32 KB) |
| `put <len>` + `<bytes>` | `OK` / `ERR` | Write `<len>` bytes to staged path A (≤32 KB) |
| `reboot` | `OK` then reboot | Restart the ESP32 |

---

## 5. NetVoid — the Flipper app

### 5.1 Build and install

Built with **ufbt** against the **Momentum SDK** (firmware **API 87.1**). `ufbt` is not on PATH in the
dev environment, so it is invoked as a module:

```powershell
cd flipper/esp_proxy
python -m ufbt            # build -> dist/esp_proxy.fap  (APPCHK prints "Target: 7, API: 87.1")
python -m ufbt launch     # build + install to /ext/apps/GPIO/esp_proxy.fap + run over USB
```

Manifest (`application.fam`): `appid="esp_proxy"`, `name="NetVoid"`, category `GPIO`,
`fap_icon="icon.png"`, `stack_size=4 KB`.

> **API compatibility:** the app's required API must match the firmware's major and be ≤ its minor.
> Momentum runs 87.1; the official SDK ships a newer API and would be rejected (`ApiTooNew`). Also note
> Momentum disables `atol()`, so the app uses its own `parse_long()`.

### 5.2 Architecture

A single-file app (`esp_proxy.c`) built on a `ViewDispatcher` with one `App` struct holding every
module and all state.

**Views (`ViewId`):** `ViewMenu` (main), `ViewList` (AP settings / Wi-Fi / Files share one Submenu),
`ViewActions` (per-file actions), `ViewText` (TextInput), `ViewInfo` (Status via VariableItemList),
`ViewPopup` (confirmations), `ViewMonitor` (serial monitor TextBox).

**Key `App` fields:** the `Uart*`; the submenus/inputs; `view`/`mode`/`tx` state machine;
`popup_ret` (where a popup's Back returns); the Wi-Fi scan arrays (`nets`, `net_open`, `net_saved`,
`net_count`); the Files arrays (`path`, `ents`, `ent_dir`, `sel`, `edit_buf`); and the serial-monitor
state (`log[]`, `log_len`, `storage`, `logfile`, `logging`, `timer`).

A **150 ms periodic `FuriTimer`** pokes the dispatcher with a custom event (`EVT_DRAIN`) so the serial
capture is drained on the dispatcher thread no matter which screen is open. A second custom event
(`EVT_WIFISCAN`) runs the Wi-Fi scan after the page is already shown.

### 5.3 The UART layer

`uart.c` / `uart.h` wrap the Flipper's GPIO USART (pin 13 TX / 14 RX, 115200 8N1):

- Acquires the USART via `furi_hal_serial_control_acquire(FuriHalSerialIdUsart)`. The **Expansion
  service owns the USART by default**, so the app disables it on open (`expansion_disable`) and
  re-enables it on close.
- The async RX ISR callback copies every byte into **two** stream buffers:
  - `rx` (4 KB) — the command channel, drained by the protocol helpers; cleared by `uart_flush()`
    before each request.
  - `log` (8 KB) — the **serial-monitor capture**, which `uart_flush()` deliberately does *not*
    touch, so the monitor keeps every byte including unsolicited firmware output.
- API: `uart_open/close`, `uart_send` (appends `\n`), `uart_send_raw` (file bytes), `uart_read_line`,
  `uart_read_bytes`, `uart_flush`, and `uart_read_log` (non-blocking drain of the capture).

### 5.4 Protocol client helpers

- `proto_ok(cmd, reply, sz)` — flush, send, scan up to 30 lines for `OK`/`ERR`; returns success and
  optionally the reply line. On silence: `"no response (check wiring/power)"`.
- `stage_path(letter, path)` — send `pa`/`pb`.
- `fetch_stat(&Stat)` — send `stat`, parse the `S <key> <val>` block into a `Stat` struct.
- `get_file(path)` — stage + `get`, read the `G <len>` header then exactly `<len>` bytes into
  `edit_buf`.
- `parse_long()` — manual integer parse (Momentum disables `atol`).

### 5.5 Screens

**Main menu** (`build_menu`): AP settings · Home Wi-Fi · Files · Status · Serial monitor · SD log
toggle · About. (Start/Stop AP are intentionally **not** duplicated here — they live in AP settings.)

**AP settings** (`scene_ap`): Rename AP · Set password · Hidden toggle (shows ON/off) · **Start AP** ·
Stop AP. Start AP jumps straight into the serial monitor instead of a popup. After Rename / Set
password / Stop, pressing Back returns to **AP settings** (not the main menu).

**Home Wi-Fi** (`scene_wifi` + `wifi_populate`): the page opens **instantly** with a "(scanning)"
header and the "+ Manual / hidden" and "Forget home Wi-Fi" items, then the networks fill in a moment
later (the blocking `wscan` runs from `EVT_WIFISCAN` after the view is shown). Saved network is marked
`*`, secured networks `#`. Selecting a secured network asks for a password; manual entry supports
hidden SSIDs.

**Files** (`scene_files`, `file_actions`): browse folders (dirs first), create files/folders. Per
entry: Open (dir) · Edit (text ≤2 KB) · Set as home page · Rename · Delete. Edit loads the file into a
TextInput and `put`s it back on save.

**Status** (`scene_status`): AP name, AP running, Internet, Home Wi-Fi, IP, SD card, Home page, Last
reset.

### 5.6 Navigation model

Global Back is handled by `back_cb`:

- Main menu → exit app.
- File actions → back to the file list.
- A list in Files (not root) → up one folder.
- A **popup** → returns to wherever it came from via `popup_ret` (`RetMenu` / `RetAp` / `RetFiles`),
  so edits inside a submenu return to that submenu.

### 5.7 Serial monitor and SD logging

- **Serial monitor** (`ViewMonitor`): a live, auto-tailing TextBox of everything the ESP32 sends. The
  capture runs continuously in the background (the 150 ms timer drains the UART log buffer into the
  RAM ring on the dispatcher thread), so you can leave the monitor and return with the history intact.
  Opening it also pulls any backlog immediately. The RAM ring keeps the most recent ~8 KB.
- **SD log toggle**: writes the whole session to the **Flipper's** SD card at
  `/ext/apps_data/esp_proxy/serial_<YYYYMMDD_HHMMSS>.txt`. Turning it on seeds the file with the
  history captured so far, then appends live; turning it off closes the file.

### 5.8 Name and icon

The app is named **NetVoid** (manifest, main-menu header, and About). The icon is a 10×10 1-bit
`icon.png`. A larger source image is converted to 1-bit by thresholding; because the Flipper draws
black pixels on a light screen, a black-on-black drawing is inverted so the intended shape becomes the
drawn (black) pixels.

> The **app-list icon cannot be animated** — the Flipper launcher only animates icons for built-in
> main-menu apps (like Infrared, which ships ~6 frames + a `frame_rate` file). External FAPs get a
> single static frame. An animation could only play *inside* the app (e.g. a launch splash).

---

## 6. End-to-end flows

**Browse a website**
1. Join the ESP32's open AP → captive menu opens (or `http://192.168.4.1/`).
2. Tap **Browser** → address bar at `http://proxy.esp32/`.
3. Type an address → ESP32 does the HTTPS request, strips restrictions, streams it back over HTTP.

**Set a custom MAC (NetVoid or web UI)**
1. `setmac AA:BB:CC:DD:EE:FF` (NetVoid) or `mac=AA:BB:…` (serial) or `/api/mac` (web).
2. Firmware kicks all soft-AP clients → applies the MAC → retries once → re-begins the home-Wi-Fi
   connection. Reply `OK <mac>`.

**Edit an SD file from the Flipper**
1. Files → pick a file → Edit (`get` fetches ≤2 KB into the editor).
2. Save → `put <len>` + raw bytes → `OK` → Back returns to the Files list.

**Connect home Wi-Fi from the Flipper**
1. Home Wi-Fi → page opens instantly, networks load in.
2. Pick one (enter password if secured) → `staname`/`stapass`/`staconn` → "connecting…".

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| NetVoid: "no response (check wiring/power)" | UART wiring (13/14/GND), baud, or ESP32 unpowered. Check the Serial monitor. |
| Custom MAC won't apply | A device held that MAC on the AP — fixed by the kick-all-clients step; retry. |
| Page load disconnects the ESP32 | Brownout (stronger USB/cable) or heavy page (OOM). `status` → `last reset:`. |
| Site shows a bot/Cloudflare wall | Expected; the proxy can't pass some bot checks. |
| Phone can't resolve names | Disable Private DNS / DNS-over-HTTPS while connected; turn off mobile data. |
| SD "empty"/unreadable | SPI too fast for the wiring; `sdMount()` steps the clock down and verifies — reseat the card in the ESP32. |
| `ufbt: command not found` | Use `python -m ufbt` (not bare `ufbt`). |
| App rejected (`ApiTooNew`) | Build against the Momentum SDK (API 87.1), not the newer official SDK. |

## 8. Limits and future work

- The client↔ESP32 link is plain HTTP over an open AP — **don't sign in or pay** through it.
- ≤2 concurrent HTTPS upstreams, TLS 1.2 only, no HTTP/2, service workers, WebCrypto subtle, camera or
  DRM video. Sites that check their own hostname in JS may break.
- **Not yet built:** flashing the ESP32 firmware *from the Flipper* over GPIO. The existing **ESP
  Flasher** Flipper app already does this; the plan is to emit the bootloader/partition/app `.bin`
  files onto the SD card so ESP Flasher can pick them up (no PC needed).
