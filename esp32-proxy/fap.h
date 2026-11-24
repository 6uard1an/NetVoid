// Machine protocol for the Flipper Zero control app (esp_proxy.fap), spoken over the same 115200 UART
// as the human console. Every command is one line; the app stages text/paths with pa/pb/apname/... so
// values may contain spaces, and files transfer with an explicit byte length. Replies: "OK", "OK <x>",
// "ERR <msg>", or a block ending in a lone "." line.
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// staged values, set by their own commands so the argument can be arbitrary text
static std::string fapPathA, fapPathB, fapSsid, fapPass;

static void fapReadBytes(char *buf, size_t n) {  // blocking read of exactly n bytes from the UART
  size_t got = 0;
  uint32_t t = millis();
  while (got < n && millis() - t < 8000) {
    int c = Serial.read();
    if (c < 0) continue;
    buf[got++] = (char)c;
    t = millis();
  }
}

static void fapStatus() {
  bool conn = WiFi.status() == WL_CONNECTED;
  uint8_t m[6];
  esp_wifi_get_mac(WIFI_IF_STA, m);
  Serial.printf("S apname %s\n", apSsid.c_str());
  Serial.printf("S appass %d\n", apPass.empty() ? 0 : 1);
  Serial.printf("S apon %d\n", apStarted ? 1 : 0);
  Serial.printf("S aphidden %d\n", apHidden ? 1 : 0);
  Serial.printf("S staname %s\n", staSsid.c_str());
  Serial.printf("S staok %d\n", conn ? 1 : 0);
  Serial.printf("S ip %s\n", conn ? WiFi.localIP().toString().c_str() : "");
  Serial.printf("S rssi %d\n", conn ? (int)WiFi.RSSI() : 0);
  Serial.printf("S mac %s\n", macStr(m).c_str());
  Serial.printf("S custommac %d\n", prefs.getBytesLength("mac") == 6 ? 1 : 0);
  Serial.printf("S sd %d\n", hasSD ? 1 : 0);
  Serial.printf("S home %s\n", homePage.c_str());
  Serial.printf("S reset %s\n", resetName(bootReason));
  Serial.println("S.");
}

static void fapScan() {
  int n = WiFi.scanNetworks(false, false);
  for (int i = 0; i < n && i < 40; i++) {
    std::string ss = WiFi.SSID(i).c_str();
    if (ss.empty()) continue;
    int rssi = WiFi.RSSI(i);
    int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -72 ? 2 : rssi >= -82 ? 1 : 0;
    Serial.printf("N %d %d %d %d %s\n", rssi, bars, WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 1 : 0, ss == staSsid ? 1 : 0, ss.c_str());
  }
  WiFi.scanDelete();
  Serial.println("N.");
}

static void fapList(const std::string &path) {
  struct stat st;
  if (sdKind(path, &st) != 2) {
    Serial.println("ERR not a folder");
    return;
  }
  DIR *d = opendir(real(path).c_str());
  if (!d) {
    Serial.println("ERR cannot open");
    return;
  }
  std::string base = path;
  if (base.back() != '/') base += '/';
  while (dirent *e = readdir(d)) {
    bool dir = e->d_type == DT_DIR;
    long sz = 0;
    if (!dir && stat((real(path) + "/" + e->d_name).c_str(), &st) == 0) sz = st.st_size;
    Serial.printf("E %c %ld %s\n", dir ? 'd' : 'f', sz, e->d_name);
  }
  closedir(d);
  Serial.println("E.");
}

// Applies the current AP settings live if the AP is already up.
static void fapReapplyAp() {
  if (apStarted) {
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), AP_IP);
    WiFi.softAP(apSsid.c_str(), apPass.empty() ? nullptr : apPass.c_str(), 1, apHidden ? 1 : 0, 4);
  }
}

// Returns true if the line was a Flipper-app command (handled here).
static bool fapCmd(const std::string &s) {
  size_t sp = s.find(' ');
  std::string k = s.substr(0, sp), v = sp == std::string::npos ? "" : s.substr(sp + 1);

  if (k == "F?") return Serial.println("F! esp32-proxy 1"), true;  // handshake + protocol version
  if (k == "stat") return fapStatus(), true;
  if (k == "wscan") return fapScan(), true;

  // ---- staging ----
  if (k == "pa") return fapPathA = v, Serial.println("OK"), true;
  if (k == "pb") return fapPathB = v, Serial.println("OK"), true;
  if (k == "staname") return fapSsid = v, Serial.println("OK"), true;
  if (k == "stapass") return fapPass = v, Serial.println("OK"), true;

  // ---- access point ----
  if (k == "apname") {
    if (v.empty() || v.size() > 31) return Serial.println("ERR name 1-31 chars"), true;
    apSsid = v;
    prefs.putString("ap", apSsid.c_str());
    fapReapplyAp();
    return Serial.println("OK"), true;
  }
  if (k == "appass") {
    if (!v.empty() && v.size() < 8) return Serial.println("ERR password must be empty or 8+ chars"), true;
    if (v.size() > 63) return Serial.println("ERR password too long"), true;
    apPass = v;
    prefs.putString("appw", apPass.c_str());
    fapReapplyAp();
    return Serial.println("OK"), true;
  }
  if (k == "aphidden") {
    apHidden = v == "1";
    prefs.putBool("aphidden", apHidden);
    fapReapplyAp();
    return Serial.println("OK"), true;
  }
  if (k == "apstart") {
    if (!apStarted) startAp(apSsid, apPass);
    return Serial.println("OK"), true;  // startAp prints human lines but no OK; the app waits for this
  }
  if (k == "apstop") {
    WiFi.softAPdisconnect(true);
    apStarted = false;
    prefs.putBool("apup", false);
    return Serial.println("OK"), true;
  }

  // ---- home Wi-Fi ----
  if (k == "staconn") {
    if (fapSsid.empty() || fapSsid.size() > 32 || fapPass.size() > 63) return Serial.println("ERR bad ssid/pass"), true;
    saveSta(fapSsid, fapPass);
    staSsid = fapSsid;
    staPass = fapPass;
    WiFi.disconnect(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(staSsid.c_str(), staPass.empty() ? nullptr : staPass.c_str());
    return Serial.println("OK"), true;
  }
  if (k == "staforget") {
    saveSta("", "");
    staSsid = staPass = "";
    WiFi.disconnect(false);
    return Serial.println("OK"), true;
  }
  if (k == "setmac") {
    std::string out, err = setMac(v, out);
    Serial.println(err.empty() ? ("OK " + out).c_str() : ("ERR " + err).c_str());
    return true;
  }

  // ---- files (operate on staged paths) ----
  if (k == "ls") return fapList(v.empty() ? fapPathA : v), true;
  if (k == "mkdir") {
    if (!hasSD || !sdPath(fapPathA)) return Serial.println("ERR bad path"), true;
    Serial.println(mkdir(real(fapPathA).c_str(), 0777) == 0 ? "OK" : "ERR cannot create");
    return true;
  }
  if (k == "rm") {
    struct stat st;
    if (!hasSD || !sdPath(fapPathA) || real(fapPathA) == SD_ROOT || sdKind(fapPathA, &st) == 0) return Serial.println("ERR bad path"), true;
    sdDelete(real(fapPathA));
    return Serial.println("OK"), true;
  }
  if (k == "mv") {
    if (!hasSD || !sdPath(fapPathA) || !sdPath(fapPathB)) return Serial.println("ERR bad path"), true;
    Serial.println(rename(real(fapPathA).c_str(), real(fapPathB).c_str()) == 0 ? "OK" : "ERR cannot rename");
    return true;
  }
  if (k == "sethome") {
    if (fapPathA.empty() || fapPathA[0] != '/' || fapPathA.size() > 128) return Serial.println("ERR bad path"), true;
    homePage = fapPathA;
    prefs.putString("home", homePage.c_str());
    return Serial.println("OK"), true;
  }
  if (k == "get") {  // read staged pathA -> "G <len>\n<bytes>"
    struct stat st;
    if (!hasSD || sdKind(fapPathA, &st) != 1) return Serial.println("ERR not a file"), true;
    if (st.st_size > 32768) return Serial.println("ERR file too large to edit"), true;
    int f = open(real(fapPathA).c_str(), O_RDONLY);
    if (f < 0) return Serial.println("ERR cannot open"), true;
    Serial.printf("G %ld\n", (long)st.st_size);
    char buf[512];
    int r;
    while ((r = read(f, buf, sizeof(buf))) > 0) Serial.write((uint8_t *)buf, r);
    close(f);
    return true;
  }
  if (k == "put") {  // write staged pathA, v = byte count that follows
    long len = atol(v.c_str());
    if (!hasSD || !sdPath(fapPathA) || len < 0 || len > 32768) return Serial.println("ERR bad path or size"), true;
    int f = open(real(fapPathA).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return Serial.println("ERR cannot write"), true;
    char buf[512];
    long left = len;
    bool ok = true;
    while (left > 0 && ok) {
      size_t chunk = left < (long)sizeof(buf) ? left : sizeof(buf);
      fapReadBytes(buf, chunk);
      ok = write(f, buf, chunk) == (int)chunk;
      left -= chunk;
    }
    close(f);
    return Serial.println(ok ? "OK" : "ERR write failed"), true;
  }
  if (k == "reboot") {
    Serial.println("OK");
    delay(100);
    ESP.restart();
  }
  return false;
}
