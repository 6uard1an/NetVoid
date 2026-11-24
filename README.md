# ESP32 web proxy

The ESP32 serves the SD card (games and apps) exactly as the original sketch did, and the menu's **Browser** app opens the proxy.

1. Join the ESP32's Wi-Fi. The menu opens by itself (or visit `http://192.168.4.1/`, or `http://<wifi-name>.local/`).
2. Tap **Browser** for the proxy's address bar (`http://proxy.esp32/`). First time only: run `connect-wifi.ps1` with the board
   on USB, or enter your home Wi-Fi there.
3. Type an address. The ESP32 makes the HTTPS request itself and serves everything back over HTTP.

`https://example.com/a` becomes `http://example.com.esp32/a`. The ESP32 answers DNS for every name.
SD routes: files and folder listings, `/list?dir=`, and `/edit` (POST upload, PUT create, DELETE). Websites opened through
the proxy cannot use `/edit`. The card is on CS pin 5. It is mounted at power-on.

Build and flash: `python build.py COM8`
Serial console (115200): `status`, `setwifi=Name;password`, `setap=Name;password`, `reset` (restart), `factory` (clear settings), `selftest`, `lan` (testing only: opens the proxy to your home network until restart).
Holding BOOT for 5 seconds clears all settings.

Restore the previous firmware:
`esptool --port COM8 write-flash 0 backup/original-flash.bin`

## Removed
CSP (header and meta), X-Frame-Options, COOP/COEP/CORP, Permissions-Policy, Referrer-Policy, HSTS, nosniff and
subresource integrity. CORS is open to all. Reading another site *with your cookies* still needs that site's consent,
so that one page cannot read your data on other sites.

## Speed
Transmit power is limited to 11 dBm (`TX_POWER`): at full power this board corrupts its own reception. Scripts, data and
most stylesheets pass through compressed; pages are unpacked on the board using spare instruction RAM (`gunzip.h`).
Scripts, styles, images and fonts are marked cacheable (1 to 7 days) and reloads of them are answered locally; use a forced
reload (Ctrl+F5) to fetch fresh copies. Images load as you scroll. Browser connections are reused, TLS sessions are resumed,
and RSA certificates are requested first because the chip checks them in hardware (0.5 s instead of 1.8 s).
Serial command `scan` lists how crowded each channel is. Ads and telemetry are answered locally (`isTracker` in proxy.cpp).
`lowmem.cpp` patches the prebuilt libraries through linker flags in `build.py`, so build with that rather than the Arduino IDE.


## If it disconnects
A heavy page can reset the ESP32 (usually a brownout on weak USB power, sometimes low memory). The access point now
restarts itself after a crash, so you reconnect without re-sending setap. Run `status` over serial to see `last reset:` —
"brownout" means use a stronger USB charger/cable; "crash (out of memory?)" means the page was too heavy. `crashtest` over
serial forces a crash to check recovery.

## Limits
- The link to the ESP32 is plain HTTP: the Wi-Fi is open by default, so anyone nearby can read it. Don't sign in or pay.
- At most two HTTPS connections at once, memory permitting (no PSRAM). WebSockets are dropped when a page load needs the connection.
- No HTTP/2 or TLS 1.3 (the prebuilt core is TLS 1.2), no service workers, WebCrypto subtle, camera or DRM video.
- Bot checks (Cloudflare and similar) can reject it, and sites that check their own hostname in JavaScript may break.
- Phones with strict Private DNS / DNS-over-HTTPS must turn it off while connected. Turn off mobile data if needed.
- Addresses on your own LAN are blocked (`ALLOW_PRIVATE` in proxy.cpp).
