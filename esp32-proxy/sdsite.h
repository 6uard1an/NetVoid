// The SD card web server: same routes and behaviour as the original sketch, on the proxy's socket server.
// Included by proxy.cpp after its request and reply helpers.
#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SD_CS 5
#define SD_ROOT "/sd"
// hasSD is defined in proxy.cpp (portal(), which precedes this include, needs it)

#ifndef SD_TEST
// The original firmware ran the card at ~4 MHz. 20 MHz mounts but reads fail on many boards' wiring
// (the card is "detected" yet every file is missing), so try slower speeds and keep the first that can
// actually open the root directory, not just initialize.
static bool sdMount() {
  for (uint32_t hz : {8000000u, 4000000u, 2000000u, 1000000u, 400000u}) {
    if (SD.begin(SD_CS, SPI, hz, SD_ROOT, 4)) {
      DIR *d = opendir(SD_ROOT);
      if (d) {
        closedir(d);
        Serial.printf("SD mounted and readable at %u kHz\n", (unsigned)(hz / 1000));
        return true;
      }
      Serial.printf("SD detected at %u kHz but not readable, trying slower\n", (unsigned)(hz / 1000));
      SD.end();
    }
    delay(30);
  }
  return false;
}
#endif

static const char *mimeOf(const std::string &path) {
  static const char *const table[][2] = {
      {"text/html", " html htm xhtml php asp aspx jsp cgi pl "},
      {"text/xml", " xml "},
      {"application/rss+xml", " rss atom "},
      {"text/css", " css scss sass less "},
      {"application/javascript", " js mjs ts tsx jsx coffee "},
      {"application/json", " json "},
      {"image/jpeg", " jpg jpeg "},
      {"image/png", " png "},
      {"image/gif", " gif "},
      {"image/webp", " webp "},
      {"image/avif", " avif "},
      {"image/svg+xml", " svg "},
      {"image/x-icon", " ico "},
      {"image/bmp", " bmp "},
      {"image/tiff", " tif tiff "},
      {"image/vnd.ms-dds", " dds "},
      {"font/woff", " woff "},
      {"font/woff2", " woff2 "},
      {"font/ttf", " ttf "},
      {"font/otf", " otf "},
      {"application/vnd.ms-fontobject", " eot "},
      {"audio/mpeg", " mp3 "},
      {"audio/wav", " wav "},
      {"audio/ogg", " ogg "},
      {"audio/aac", " aac "},
      {"audio/mp4", " m4a "},
      {"video/mp4", " mp4 "},
      {"video/webm", " webm "},
      {"video/quicktime", " mov "},
      {"video/x-msvideo", " avi "},
      {"video/x-flv", " flv "},
      {"video/x-matroska", " mkv "},
      {"video/3gpp", " 3gp "},
      {"application/pdf", " pdf "},
      {"application/msword", " doc "},
      {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", " docx "},
      {"application/vnd.ms-excel", " xls "},
      {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", " xlsx "},
      {"application/vnd.ms-powerpoint", " ppt "},
      {"application/vnd.openxmlformats-officedocument.presentationml.presentation", " pptx "},
      {"application/rtf", " rtf "},
      {"text/csv", " csv "},
      {"application/toml", " toml "},
      {"application/x-yaml", " yaml yml "},
      {"application/zip", " zip "},
      {"application/vnd.rar", " rar "},
      {"application/x-7z-compressed", " 7z "},
      {"application/x-tar", " tar "},
      {"application/gzip", " gz "},
      {"application/manifest+json", " webmanifest "},
      {"application/octet-stream", " unityweb data mem pck bundle "},
      {"application/wasm", " wasm "},
      {"model/3d", " obj fbx gltf glb "},
  };
  size_t dot = path.rfind('.');
  if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return "text/plain";
  std::string e = " " + path.substr(dot + 1) + " ";
  for (auto &row : table)
    if (strstr(row[1], e.c_str())) return row[0];
  return "text/plain";
}

// Unlike reply(), no cross-origin header: proxied websites must not be able to read the card.
static void sdReply(int fd, const char *status, const char *type, const std::string &body) {
  sendStr(fd, std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-cache, no-store, must-revalidate\r\n" + connHeader() + "\r\n" + body);
}

static std::string pctDecode(const std::string &s) {  // like urlDecode, but keeps '+'
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}
static std::string pctEncode(const std::string &s) {
  std::string o;
  char b[4];
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~/!()*,", c)) o += c;
    else {
      snprintf(b, sizeof(b), "%%%02X", c);
      o += b;
    }
  }
  return o;
}
static std::string htmlEsc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static bool sdPath(const std::string &p) { return !p.empty() && p[0] == '/' && p.find("..") == std::string::npos && p.find('\0') == std::string::npos && p.size() < 400; }
static std::string real(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p == "/" ? SD_ROOT : SD_ROOT + p;
}
// 0 = missing, 1 = file, 2 = directory
static int sdKind(const std::string &p, struct stat *st) {
  if (!hasSD || !sdPath(p)) return 0;
  if (p == "/") return 2;
  if (stat(real(p).c_str(), st) != 0) return 0;
  return S_ISDIR(st->st_mode) ? 2 : 1;
}

static bool sdFile(int fd, Worker *w, const Req &r, std::string path) {
  const char *type = mimeOf(path);
  if (path.size() > 4 && path.compare(path.size() - 4, 4, ".src") == 0) {
    path.resize(path.size() - 4);
    type = "text/plain";
  }
  struct stat st;
  if (sdKind(path, &st) != 1) return false;
  char tag[40];
  snprintf(tag, sizeof(tag), "\"%lx-%lx\"", (unsigned long)st.st_size, (unsigned long)st.st_mtime);
  // Always rechecked, so edits show at once; an unchanged file is not sent again.
  std::string common = std::string("ETag: ") + tag + "\r\nCache-Control: no-cache\r\nAccept-Ranges: bytes\r\n" + connHeader();
  if (r.inm == tag) return sendStr(fd, "HTTP/1.1 304 Not Modified\r\n" + common + "\r\n"), true;
  long size = st.st_size, from = 0, to = size - 1;
  bool part = false;
  if (r.range.compare(0, 6, "bytes=") == 0 && r.range.find(',') == std::string::npos && size > 0) {
    const char *p = r.range.c_str() + 6;
    char *e;
    if (*p == '-') {
      long n = atol(p + 1);
      from = n >= size ? 0 : size - n;
    } else {
      from = strtol(p, &e, 10);
      if (*e == '-' && e[1]) to = atol(e + 1);
    }
    if (to >= size) to = size - 1;
    part = from >= 0 && from <= to;
    if (!part) return sendStr(fd, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nContent-Range: bytes */" + std::to_string(size) + "\r\n" + common + "\r\n"), true;
  }
  int f = open(real(path).c_str(), O_RDONLY);
  for (int i = 0; f < 0 && i < 20; i++) {
    delay(25);
    f = open(real(path).c_str(), O_RDONLY);
  }
  if (f < 0) return false;
  long left = to - from + 1;
  std::string h = std::string(part ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") + "Content-Type: " + type + "\r\nContent-Length: " + std::to_string(left) + "\r\n";
  if (part) h += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
  bool ok = sendStr(fd, h + common + "\r\n");
  if (r.method != "HEAD") {
    if (from) lseek(f, from, SEEK_SET);
    while (ok && left > 0) {
      int n = read(f, w->buf, left < BUF ? left : BUF);  // the request has been parsed out of buf by now
      if (n <= 0) keepOpen = ok = false;
      else {
        ok = sendAll(fd, w->buf, n);
        left -= n;
      }
    }
  }
  close(f);
  return true;
}

static void sdIndex(int fd, Worker *w, const Req &r) {
  if (hasSD && sdFile(fd, w, r, homePage)) return;
  sdReply(fd, "200 OK", "text/html", "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Portal</title></head><body><h1>Portal</h1><p>No SD card found.</p><p><a href='http://proxy.esp32/'>Browser</a></p></body></html>");
}

static bool chunk(int fd, const std::string &s) {
  char n[12];
  snprintf(n, sizeof(n), "%x\r\n", (unsigned)s.size());
  return s.empty() || sendStr(fd, n + s + "\r\n");
}

static void sdListing(int fd, std::string path) {
  DIR *d = opendir(real(path).c_str());
  if (!d) return sdReply(fd, "404 Not Found", "text/plain", "Not a directory");
  if (path.back() != '/') path += '/';
  sendStr(fd, std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n") + connHeader() + "\r\n");
  std::string o = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Index of " + htmlEsc(path) +
                  "</title><style>body{font-family:system-ui;background:#0e0e0e;color:#eee;padding:16px}a{color:#7fb7ff;text-decoration:none}"
                  "a:hover{text-decoration:underline}li{margin:6px 0}</style></head><body><h2>Index of " + htmlEsc(path) + "</h2><ul>";
  if (path != "/") o += "<li><a href='" + pctEncode(path.substr(0, path.rfind('/', path.size() - 2) + 1)) + "'>..</a></li>";
  bool ok = true;
  while (dirent *e = readdir(d)) {
    bool dir = e->d_type == DT_DIR;
    std::string n = e->d_name;
    o += "<li><a href='" + pctEncode(path + n) + (dir ? "/" : "") + "'>" + (dir ? "&#128193; " : "&#128196; ") + htmlEsc(n) + "</a></li>";
    if (o.size() > 1200) {
      if (!(ok = chunk(fd, o))) break;
      o.clear();
    }
  }
  closedir(d);
  if (ok) chunk(fd, o + "</ul></body></html>") && sendAll(fd, "0\r\n\r\n", 5);
}

static void sdList(int fd, const std::string &dir) {
  struct stat st;
  int k = sdKind(dir, &st);
  if (!k) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD PATH\r\n");
  DIR *d = k == 2 ? opendir(real(dir).c_str()) : nullptr;
  if (!d) return sdReply(fd, "500 Internal Server Error", "text/plain", "NOT DIR\r\n");
  sendStr(fd, std::string("HTTP/1.1 200 OK\r\nContent-Type: text/json\r\nTransfer-Encoding: chunked\r\n") + connHeader() + "\r\n");
  std::string o = "[", base = dir;
  if (base.back() != '/') base += '/';
  bool first = true, ok = true;
  while (dirent *e = readdir(d)) {
    if (!first) o += ',';
    first = false;
    o += std::string("{\"type\":\"") + (e->d_type == DT_DIR ? "dir" : "file") + "\",\"name\":\"" + base + e->d_name + "\"}";
    if (o.size() > 1200) {
      if (!(ok = chunk(fd, o))) break;
      o.clear();
    }
  }
  closedir(d);
  if (ok) chunk(fd, o + "]") && sendAll(fd, "0\r\n\r\n", 5);
}

static void sdDelete(const std::string &p) {
  DIR *d = opendir(p.c_str());
  if (!d) {
    unlink(p.c_str());
    return;
  }
  std::vector<std::pair<std::string, bool>> names;
  while (dirent *e = readdir(d)) names.emplace_back(e->d_name, e->d_type == DT_DIR);
  closedir(d);
  for (auto &n : names) {
    if (n.second) sdDelete(p + "/" + n.first);
    else unlink((p + "/" + n.first).c_str());
  }
  rmdir(p.c_str());
}

// Streams a multipart/form-data upload to the card.
static void sdUpload(int fd, Worker *w, const Req &r, const std::string &first) {
  keepOpen = false;
  size_t b = r.ctype.find("boundary=");
  if (b == std::string::npos || r.chunked) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD ARGS\r\n");
  std::string bound = r.ctype.substr(b + 9);
  bound = bound.substr(0, bound.find(';'));
  if (bound.size() > 1 && bound[0] == '"') bound = bound.substr(1, bound.find('"', 1) - 1);
  const std::string delim = "\r\n--" + bound;
  std::string buf = "\r\n" + first;  // so the opening boundary looks like the others
  long left = r.bodyLen - (long)first.size();
  int f = -1;
  bool inHead = false, ok = true, done = false;
  while (ok && !done) {
    bool progress = true;
    while (progress && ok && !done) {
      progress = false;
      if (inHead) {
        size_t e = buf.find("\r\n\r\n");
        if (buf.compare(0, 2, "--") == 0) done = true;
        else if (e != std::string::npos) {
          size_t q = buf.find("filename=\"");
          if (q < e) {
            std::string name = buf.substr(q + 10, buf.find('"', q + 10) - q - 10);
            if (!name.empty() && name[0] != '/') name = "/" + name;
            if (sdPath(name) && name.size() > 1) {
              unlink(real(name).c_str());
              f = open(real(name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
              ok = f >= 0;
            } else ok = false;
          }
          buf.erase(0, e + 4);
          inHead = false;
          progress = true;
        } else if (buf.size() > 4096) ok = false;
      } else {
        size_t e = buf.find(delim);
        size_t n = e != std::string::npos ? e : buf.size() > delim.size() ? buf.size() - delim.size() : 0;
        if (n && f >= 0) ok = write(f, buf.data(), n) == (int)n;
        buf.erase(0, n);
        if (e != std::string::npos) {
          if (f >= 0) close(f);
          f = -1;
          buf.erase(0, delim.size());
          inHead = progress = true;
        }
      }
    }
    if (done || !ok) break;
    if (left <= 0) ok = false;
    else {
      int g = recv(fd, w->buf, left < 4096 ? left : 4096, 0);
      if (g <= 0) ok = false;
      else {
        buf.append(w->buf, g);
        left -= g;
      }
    }
  }
  if (f >= 0) close(f);
  if (ok) sdReply(fd, "200 OK", "text/plain", "");
  else sdReply(fd, "500 Internal Server Error", "text/plain", "UPLOAD FAILED\r\n");
}


// ---------- Wi-Fi + MAC control API (same-origin only; proxied pages are blocked) ----------
static std::string jsonEsc(const std::string &s) {
  std::string o;
  char b[8];
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) snprintf(b, sizeof(b), "\\u%04x", c), o += b;
    else o += c;
  }
  return o;
}
static std::string macStr(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}
static void apiSend(int fd, const char *status, const std::string &json) {
  sendStr(fd, std::string("HTTP/1.1 ") + status + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n" + connHeader() + "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
}

// "" / "default" restores the factory MAC, "random" makes one up, else expects AA:BB:CC:DD:EE:FF.
// Returns "" on success (and sets outMac), or a human message on failure.
static std::string setMac(std::string spec, std::string &outMac) {
  for (auto &c : spec) {
    if (c == '-') c = ':';
    c = tolower((unsigned char)c);
  }
  uint8_t mac[6];
  bool custom = true;
  if (spec.empty() || spec == "default") custom = false;
  else if (spec == "random") {
    esp_fill_random(mac, 6);
    mac[0] = (mac[0] & 0xFC) | 0x02;  // unicast, locally administered
  } else {
    unsigned v[6];
    if (sscanf(spec.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return "Use the form AA:BB:CC:DD:EE:FF.";
    for (int i = 0; i < 6; i++) {
      if (v[i] > 255) return "Each part must be 00-FF.";
      mac[i] = v[i];
    }
    if (mac[0] & 1) return "The first part must be even (a unicast address).";
  }
  WiFi.disconnect(false);
  esp_err_t e;
  if (custom) {
    // The chip refuses a STA MAC that a soft-AP client is currently using. Kick the
    // AP clients (incl. any holding this exact MAC) to free it, then apply; retry once.
    esp_wifi_deauth_sta(0);  // 0 = all associated stations
    delay(150);
    e = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (e != ESP_OK) {
      esp_wifi_deauth_sta(0);
      delay(300);
      e = esp_wifi_set_mac(WIFI_IF_STA, mac);
    }
  } else {
    uint8_t def[6];
    esp_read_mac(def, ESP_MAC_WIFI_STA);
    e = esp_wifi_set_mac(WIFI_IF_STA, def);
    memcpy(mac, def, 6);
  }
  if (e == ESP_OK) {
    if (custom) prefs.putBytes("mac", mac, 6);
    else prefs.remove("mac");
  }
  if (!staSsid.empty()) WiFi.begin(staSsid.c_str(), staPass.empty() ? nullptr : staPass.c_str());
  outMac = macStr(mac);
  return e == ESP_OK ? "" : "The chip refused that address.";
}

static void apiStatus(int fd) {
  bool conn = WiFi.status() == WL_CONNECTED;
  uint8_t m[6];
  esp_wifi_get_mac(WIFI_IF_STA, m);
  std::string j = "{\"connected\":";
  j += conn ? "true" : "false";
  j += ",\"ssid\":\"" + jsonEsc(staSsid) + "\",\"ip\":\"" + std::string(conn ? WiFi.localIP().toString().c_str() : "") + "\",\"rssi\":" + std::to_string(conn ? WiFi.RSSI() : 0);
  j += ",\"mac\":\"" + macStr(m) + "\",\"custom_mac\":";
  j += prefs.getBytesLength("mac") == 6 ? "true" : "false";
  j += ",\"ap\":\"" + jsonEsc(apSsid) + "\",\"ap_on\":";
  j += apStarted ? "true" : "false";
  j += ",\"sd\":";
  j += hasSD ? "true" : "false";
  j += "}";
  apiSend(fd, "200 OK", j);
}

static void apiScan(int fd) {
  int n = WiFi.scanNetworks(false, false);
  std::string j = "[";
  bool first = true;
  for (int i = 0; i < n && i < 40; i++) {
    std::string ss = WiFi.SSID(i).c_str();
    if (ss.empty()) continue;
    int rssi = WiFi.RSSI(i);
    int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -72 ? 2 : rssi >= -82 ? 1 : 0;
    if (!first) j += ",";
    first = false;
    j += "{\"ssid\":\"" + jsonEsc(ss) + "\",\"rssi\":" + std::to_string(rssi) + ",\"bars\":" + std::to_string(bars) + ",\"chan\":" + std::to_string(WiFi.channel(i)) +
         ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + ",\"saved\":" + (ss == staSsid ? "true" : "false") + "}";
  }
  j += "]";
  WiFi.scanDelete();
  apiSend(fd, "200 OK", j);
}

static void apiHandle(int fd, Worker *w, const Req &r, std::string body) {
  keepOpen = false;
  // A page opened through the proxy lives on a *.esp32 origin and must not reach these controls.
  if (!r.origin.empty() && r.origin != "http://" + r.host) return apiSend(fd, "403 Forbidden", "{\"error\":\"forbidden\"}");
  size_t q = r.path.find('?');
  std::string uri = r.path.substr(0, q), query = q == std::string::npos ? "" : r.path.substr(q + 1);
  bool post = r.method == "POST";
  if (post) {
    if (r.chunked || r.bodyLen > 2048) return apiSend(fd, "413 Payload Too Large", "{\"error\":\"too large\"}");
    while ((long)body.size() < r.bodyLen) {
      int g = recv(fd, w->io, IO, 0);
      if (g <= 0) return;
      body.append(w->io, g);
    }
  }
  const std::string &src = post ? body : query;
  if (uri == "/api/status" && !post) return apiStatus(fd);
  if (uri == "/api/scan" && !post) return apiScan(fd);
  if (uri == "/api/connect" && post) {
    std::string ssid = field(src, "ssid"), pass = field(src, "password");
    if (ssid.empty() || ssid.size() > 32 || pass.size() > 63) return apiSend(fd, "400 Bad Request", "{\"error\":\"Enter a network name (and a password if it is locked).\"}");
    saveSta(ssid, pass);
    staSsid = ssid;
    staPass = pass;
    WiFi.disconnect(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.empty() ? nullptr : pass.c_str());
    return apiSend(fd, "200 OK", "{\"ok\":true}");
  }
  if (uri == "/api/mac" && post) {
    std::string out, err = setMac(field(src, "mac"), out);
    if (err.empty()) return apiSend(fd, "200 OK", "{\"ok\":true,\"mac\":\"" + out + "\"}");
    return apiSend(fd, "400 Bad Request", "{\"error\":\"" + jsonEsc(err) + "\"}");
  }
  if (uri == "/api/forget" && post) {
    saveSta("", "");
    staSsid = "";
    staPass = "";
    WiFi.disconnect(false);
    return apiSend(fd, "200 OK", "{\"ok\":true}");
  }
  apiSend(fd, "404 Not Found", "{\"error\":\"unknown endpoint\"}");
}

static void sdSite(int fd, Worker *w, const Req &r, std::string body) {
  size_t q = r.path.find('?');
  std::string uri = r.path.substr(0, q), query = q == std::string::npos ? "" : r.path.substr(q + 1);
  bool get = r.method == "GET" || r.method == "HEAD";
  if (uri.compare(0, 5, "/api/") == 0) return apiHandle(fd, w, r, body);
  std::string h = r.host.substr(0, r.host.find(':'));

  if (uri == "/edit" && !get) {
    // Only pages served from this card may change it: not websites being proxied.
    if (!r.origin.empty() && r.origin != "http://" + r.host) return sdReply(fd, "403 Forbidden", "text/plain", "FORBIDDEN\r\n");
    if (!hasSD) return sdReply(fd, "500 Internal Server Error", "text/plain", "NO SD CARD\r\n");
    if (r.method == "POST") return sdUpload(fd, w, r, body);
    if (r.method == "DELETE" || r.method == "PUT") {
      if (r.bodyLen > 2048) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD ARGS\r\n");
      while ((long)body.size() < r.bodyLen) {
        int g = recv(fd, w->io, IO, 0);
        if (g <= 0) return;
        body.append(w->io, g);
      }
      std::string arg = !query.empty() ? query : body;  // the first argument, whatever its name
      arg = arg.substr(0, arg.find('&'));
      size_t eq = arg.find('=');
      std::string p = urlDecode(eq == std::string::npos ? arg : arg.substr(eq + 1));
      if (p.empty()) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD ARGS\r\n");
      struct stat st;
      int k = sdKind(p, &st);
      bool del = r.method == "DELETE";
      if (!sdPath(p) || real(p) == SD_ROOT || (k != 0) != del) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD PATH\r\n");
      if (del) sdDelete(real(p));
      else if (p.find('.') != std::string::npos && p.find('.') > 0) {
        int f = open(real(p).c_str(), O_WRONLY | O_CREAT, 0666);
        if (f >= 0) close(f);
      } else mkdir(real(p).c_str(), 0777);
      return sdReply(fd, "200 OK", "text/plain", "");
    }
  }
  if (uri == "/list" && get) {
    if (("&" + query).find("&dir=") == std::string::npos) return sdReply(fd, "500 Internal Server Error", "text/plain", "BAD ARGS\r\n");
    return sdList(fd, field(query, "dir"));
  }
  if (uri == "/") return sdIndex(fd, w, r);

  // Real files first, so the sign-in pop-up can load the menu from whatever address it opened.
  struct stat st;
  for (const std::string &p : {pctDecode(uri), urlDecode(uri)}) {
    int k = sdKind(p, &st);
    if (k == 2) return sdListing(fd, p);
    if (sdFile(fd, w, r, p)) return;
  }
  for (auto x : {"/generate_204", "/gen_204", "/fwlink", "/hotspot-detect.html", "/favicon.ico", "/ncsi.txt", "/connecttest.txt", "/redirect", "/success.txt", "/canonical.html",
                 "/connectivitycheck.gstatic.com/generate_204", "/connectivitycheck.gstatic.com/generate_204//", "/library/test/success.html"})
    if (uri == x) return sdIndex(fd, w, r);
  if (h.find("google") != std::string::npos || h.find("android") != std::string::npos) return sdIndex(fd, w, r);
  sdReply(fd, "404 Not Found", "text/plain", "File not found on SD\n\nURI: " + uri + "\nMethod: " + r.method + "\n");
}
