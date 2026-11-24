// ESP32 web proxy: Wi-Fi sign-in page with an address bar.
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#ifdef SD_TEST
#include <LittleFS.h>
#endif
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_tls.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <vector>
#include <lwip/sockets.h>
#include "gunzip.h"
#include "hook_gz.h"
#include "pages.h"
#include "rewrite.h"

#define WORKERS 2
#define BUF 8192            // request headers, then response header lines
#define IO 2872
#define TLS_HEADROOM 86000  // free RAM needed for a second TLS connection; without one, a page waiting on its own script deadlocks
#define UP_TIMEOUT_MS 20000
#define ALLOW_PRIVATE 0     // 1 = let proxy users reach devices on your own LAN

// 11 dBm. Above this, this board's own transmissions corrupt what it is receiving: measured 10-60 KB/s
// at full power against 100-150 KB/s here. The unit is a quarter dBm; a better USB supply may allow more.
#define TX_POWER 44

static const IPAddress AP_IP(192, 168, 4, 1);
void tlsBuffersBegin();  // lowmem.cpp
static Preferences prefs;
static std::string apSsid, apPass, staSsid, staPass;
static int listenFd = -1, dnsFd = -1, mdnsFd = -1;
static std::string localName;  // "netvoid.local"
static QueueHandle_t jobs, idleQ;  // idleQ: kept-open connections going back to the acceptor
static SemaphoreHandle_t poolMx;
static volatile int wsCount = 0;
static volatile bool lanTest = false;  // serial "lan" command; cleared by reboot
static bool apStarted = false;  // the AP waits for a setap= command, exactly like the original Flipper sketch
static bool hasSD = false;
static int bootReason = 0;
static bool apHidden = false;        // AP broadcasts its name unless this is set
static std::string homePage = "/index.html";  // file served at "/" on the card
static uint32_t released[8];
static int releasedN = 0;

// ---------- upstream connections ----------
struct Up {
  esp_tls_t *t = nullptr;
  char host[256] = "";  // fixed, so acquiring a connection can never throw while holding locks
  int port = 0;
  bool https = false, busy = false;
  uint32_t used = 0;
};
static Up pool[2];  // the second is only opened when memory allows (TLS_HEADROOM)
static volatile int waiting = 0;
static SemaphoreHandle_t connMx;  // one handshake at a time: they are the memory peak

static void upClose(Up &u) {
  if (u.t) esp_tls_conn_destroy(u.t);
  u.t = nullptr;
}

static bool isPrivate(uint32_t a) {  // host byte order
  uint8_t b = a >> 24, c = a >> 16;
  return b == 0 || b == 10 || b == 127 || b >= 224 || (b == 169 && c == 254) || (b == 192 && c == 168) ||
         (b == 172 && (c & 0xF0) == 16) || (b == 100 && (c & 0xC0) == 64);
}

static int upFd(Up &u) {
  int fd = -1;
  if (u.t) esp_tls_get_conn_sockfd(u.t, &fd);
  return fd;
}

// An idle connection is only reusable if the server has neither closed it nor written to it.
static bool upAlive(Up &u) {
  char c;
  int fd = upFd(u);
  if (fd < 0) return false;
  int r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static Up *upAcquire(const std::string &host, int port, bool https, bool fresh, bool &reused, const char *&err) {
  uint32_t start = millis();
  Up *u = nullptr;
  while (!u) {
    if (xSemaphoreTake(connMx, pdMS_TO_TICKS(45000)) != pdTRUE) {
      err = "The proxy is busy. Try again in a moment.";
      return nullptr;
    }
    xSemaphoreTake(poolMx, portMAX_DELAY);
    if (!fresh)
      for (auto &p : pool)
        if (!u && p.t && !p.busy && p.port == port && p.https == https && p.host == host) {
          if (upAlive(p)) u = &p;
          else upClose(p);
        }
    if (u) {
      u->busy = reused = true;
      xSemaphoreGive(poolMx);
      xSemaphoreGive(connMx);
      return u;
    }
    // Make room: drop idle connections, oldest first, until there is memory and a slot.
    for (;;) {
      Up *slot = nullptr, *old = nullptr;
      bool anyBusy = false;
      for (auto &p : pool) {
        anyBusy |= p.busy;
        if (p.busy) continue;
        if (!p.t) slot = &p;
        else if (!old || (int32_t)(p.used - old->used) < 0) old = &p;
      }
      bool mem = !https || (heap_caps_get_free_size(MALLOC_CAP_8BIT) > TLS_HEADROOM && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > 17500) || (!anyBusy && !old);
      if (slot && mem) {
        u = slot;
        u->busy = true;
        break;
      }
      if (!old) break;
      upClose(*old);
    }
    xSemaphoreGive(poolMx);
    if (!u) {
      xSemaphoreGive(connMx);
      waiting = waiting + 1;
      if (millis() - start > 45000) {
        waiting = waiting - 1;
        err = "The proxy is busy. Try again in a moment.";
        return nullptr;
      }
      delay(30);
      waiting = waiting - 1;
    }
  }
  reused = false;
  strlcpy(u->host, host.c_str(), sizeof(u->host));
  u->port = port;
  u->https = https;
  static const char *alpn[] = {"http/1.1", nullptr};
  esp_tls_cfg_t cfg = {};
  cfg.timeout_ms = UP_TIMEOUT_MS;
  cfg.is_plain_tcp = !https;
  cfg.alpn_protos = alpn;
  if (https) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  // RSA certificates are checked in hardware: about 0.5 s per new site instead of 1.8 s. Sites that only have
  // ECDSA certificates refuse at once, and are then offered everything.
  static const int rsaSuites[] = {0xC02F, 0xC030, 0xCCA8, 0xC027, 0xC013, 0};
  esp_tls_t *t = nullptr;
  bool connected = false;
  for (int pass = https ? 0 : 1; pass < 2 && !connected; pass++) {
    cfg.ciphersuites_list = pass ? nullptr : rsaSuites;
    tlsBuffersBegin();
    uint32_t t0 = millis();
    t = esp_tls_init();
    connected = t && esp_tls_conn_new_sync(host.c_str(), host.size(), port, &cfg, t) == 1;
    if (!connected && t) {
      esp_tls_conn_destroy(t);
      t = nullptr;
    }
    if (millis() - t0 > 5000) break;  // unreachable, not a refusal
  }
  if (connected) {
    u->t = t;
    int fd = upFd(*u);
    sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    bool bad = getpeername(fd, (sockaddr *)&sa, &sl) != 0 || sa.ss_family != AF_INET || isPrivate(ntohl(((sockaddr_in *)&sa)->sin_addr.s_addr));
    if (!bad || ALLOW_PRIVATE) {
      timeval tv = {UP_TIMEOUT_MS / 1000, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      xSemaphoreGive(connMx);
      return u;
    }
    err = "Addresses on the proxy owner's local network are blocked.";
  } else {
    err = "Could not reach that site, or its certificate is not trusted.";
    if (t) esp_tls_conn_destroy(t);
  }
  Serial.printf("upstream failed, heap=%u max=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  xSemaphoreTake(poolMx, portMAX_DELAY);
  upClose(*u);
  u->busy = false;
  xSemaphoreGive(poolMx);
  xSemaphoreGive(connMx);
  return nullptr;
}

static void upRelease(Up *u, bool keep) {
  xSemaphoreTake(poolMx, portMAX_DELAY);
  if (!keep) upClose(*u);
  u->used = millis();
  u->busy = false;
  xSemaphoreGive(poolMx);
}

static bool tlsWait(int r) { return r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE; }

static bool upWrite(Up *u, const char *d, size_t n) {
  uint32_t t = millis();
  while (n) {
    int r = esp_tls_conn_write(u->t, d, n < 1400 ? n : 1400);  // see lowmem.cpp
    if (r > 0) d += r, n -= r;
    else if (tlsWait(r) && millis() - t < UP_TIMEOUT_MS) delay(2);
    else return false;
  }
  return true;
}
static int upRead(Up *u, char *d, size_t n) {
  uint32_t t = millis();
  for (;;) {
    int r = esp_tls_conn_read(u->t, d, n);
    if (r >= 0) return r;
    if (!tlsWait(r) || millis() - t > UP_TIMEOUT_MS) return -1;
    delay(2);
  }
}

// ---------- client side ----------
// Whether the browser's connection can carry another request after this reply.
static thread_local bool keepOpen = false;
static const char *connHeader() { return keepOpen ? "Connection: keep-alive\r\n" : "Connection: close\r\n"; }

static bool sendAll(int fd, const char *d, size_t n) {
  while (n) {
    int r = send(fd, d, n, 0);
    if (r <= 0) {
      keepOpen = false;
      return false;
    }
    d += r;
    n -= r;
  }
  return true;
}
static bool sendStr(int fd, const std::string &s) { return sendAll(fd, s.data(), s.size()); }

static void reply(int fd, const char *status, const char *type, const char *a, const char *b = "", const char *extra = "") {
  std::string h = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(strlen(a) + strlen(b)) +
                  "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n" + connHeader() + extra + "\r\n";
  if (sendStr(fd, h) && sendAll(fd, a, strlen(a))) sendAll(fd, b, strlen(b));
}
static void errorPage(int fd, const char *status, const std::string &msg) {
  std::string b = "<h1>Can't open this page</h1><p>" + msg + "<p><a href='http://proxy.esp32/'>Address bar</a>";
  reply(fd, status, "text/html", PAGE_HEAD, b.c_str());
}

struct Req {
  std::string inm, range, ctype, method, path, host, origin, referer, upgrade, acrm, acrh;
  long bodyLen = 0;
  bool chunked = false, gzip = false, html = false, close = false, revalidate = false, hard = false;
  std::string fwd;  // headers to forward verbatim
};

static Gunzip gunzip;  // one page at a time
static volatile bool gunzipBusy = false;
static Gunzip *gunzipNew() {
  if (!gunzip.win) return nullptr;
  xSemaphoreTake(poolMx, portMAX_DELAY);
  bool mine = !gunzipBusy;
  if (mine) gunzipBusy = true;
  xSemaphoreGive(poolMx);
  return mine ? &gunzip : nullptr;
}
static void gunzipFree(Gunzip *g) {
  if (g) gunzipBusy = false;
}

struct Worker {
  char buf[BUF], io[IO], cb[1440];
  int cfd;
  size_t inPos, inLen;  // unread upstream bytes in io
  Rewriter rw;
  // for "status"
  const char *volatile stage = "idle";
  volatile uint32_t bytes = 0, since = 0;
  uint32_t tRead = 0, tSend = 0;
};
static Worker *workers[WORKERS];

static std::string urlDecode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}
static std::string field(const std::string &body, const char *k) {
  std::string q = "&" + body, key = std::string("&") + k + "=";
  size_t p = q.find(key);
  if (p == std::string::npos) return "";
  p += key.size();
  return urlDecode(q.substr(p, q.find('&', p) - p));
}

static void saveSta(const std::string &s, const std::string &p) {
  prefs.putString("sta", s.c_str());
  prefs.putString("stapw", p.c_str());
}

static uint32_t peerIp(int fd) {
  sockaddr_in a = {};
  socklen_t l = sizeof(a);
  return getpeername(fd, (sockaddr *)&a, &l) == 0 ? a.sin_addr.s_addr : 0;
}

static void portal(int fd, const Req &r, const std::string &body) {
  bool setup = staSsid.empty();
  // Only the portal itself may submit its forms, not a page being proxied.
  bool same = r.method == "POST" && r.origin == "http://proxy.esp32";
  if (r.path == "/setup" && setup && same) {
    std::string s = field(body, "s"), p = field(body, "p");
    if (s.empty() || s.size() > 32 || p.size() > 63 || (!p.empty() && p.size() < 8))
      return errorPage(fd, "400 Bad Request", "The Wi-Fi name is missing, or the password is not 8 to 63 characters.");
    saveSta(s, p);
    reply(fd, "200 OK", "text/html", PAGE_HEAD, "<h1>Saved</h1><p>Restarting. Reconnect to this Wi-Fi in about 10 seconds.");
    delay(700);
    ESP.restart();
  } else if (r.path == "/release" && same) {
    released[releasedN++ % 8] = peerIp(fd);
    reply(fd, "200 OK", "text/html", PAGE_HEAD, "<h1>You can leave this screen</h1><p>Stay on this Wi-Fi and open <b>http://proxy.esp32/</b> in your browser.<p><a href='/'>Address bar</a>");
  } else if (r.path == "/" || r.path.compare(0, 2, "/?") == 0) {
    if (hasSD) {
      sendStr(fd, std::string("HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/html/Browser/index.html\r\nContent-Length: 0\r\n") + connHeader() + "\r\n");
      return;
    }
    std::string page = setup ? PAGE_SETUP : PAGE_PORTAL;
    if (!setup && WiFi.status() != WL_CONNECTED) page = "<p><b>The proxy is not connected to the internet right now.</b></p>" + page;
    reply(fd, "200 OK", "text/html", PAGE_HEAD, page.c_str());
  } else reply(fd, "404 Not Found", "text/plain", "Not found");
}

#include "sdsite.h"

static void captive(int fd, Worker *w, const Req &r, const std::string &body) {
  std::string h = r.host.substr(0, r.host.find(':'));
  // A stray protocol-relative link from a proxied page: send it through the proxy.
  if (r.referer.find(".esp32/") != std::string::npos && r.referer.find("//proxy.esp32/") == std::string::npos && validHost(h.c_str(), h.size()) && h != "192.168.4.1" && !(h.size() > 6 && h.compare(h.size() - 6, 6, ".local") == 0)) {
    keepOpen = false;
    sendStr(fd, "HTTP/1.1 307 Temporary Redirect\r\nContent-Length: 0\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nLocation: http://" + mapHost(true, h, 0) + r.path + "\r\n\r\n");
    return;
  }
  uint32_t ip = peerIp(fd);
  bool rel = false;
  for (auto x : released) rel |= ip && x == ip;
  if (rel) {
    auto has = [](const std::string &s, const char *x) { return s.find(x) != std::string::npos; };
    if (has(r.path, "generate_204") || has(r.path, "gen_204")) {
      keepOpen = false;
      sendStr(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    if (has(h, "apple.com") || has(r.path, "hotspot-detect")) return reply(fd, "200 OK", "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    if (r.path == "/connecttest.txt") return reply(fd, "200 OK", "text/plain", "Microsoft Connect Test");
    if (r.path == "/success.txt") return reply(fd, "200 OK", "text/plain", "success\n");
    if (r.path == "/canonical.html") return reply(fd, "200 OK", "text/html", "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>");
  }
  sdSite(fd, w, r, body);
}

// ---------- responses ----------
static int rdByte(Worker *w, Up *u) {
  if (w->inPos == w->inLen) {
    int r = upRead(u, w->io, IO);
    if (r <= 0) return -1;
    w->inLen = r;
    w->inPos = 0;
  }
  return (uint8_t)w->io[w->inPos++];
}
// Reads a line into w->buf. Overlong lines are truncated and flagged so the caller can drop them.
static int rdLine(Worker *w, Up *u, bool &over) {
  size_t n = 0;
  over = false;
  for (size_t total = 0;; total++) {
    int c = rdByte(w, u);
    if (c < 0 || total > 65536) return -1;
    if (c == '\n') break;
    if (n < BUF - 1) w->buf[n++] = c;
    else over = true;
  }
  while (n && w->buf[n - 1] == '\r') n--;
  w->buf[n] = 0;
  return n;
}

static bool isOneOf(const std::string &k, std::initializer_list<const char *> l) {
  for (auto s : l)
    if (k == s) return true;
  return false;
}

static bool dropHeader(const std::string &k) {
  return isOneOf(k, {// content restriction policies
                     "content-security-policy", "content-security-policy-report-only", "x-content-security-policy", "x-webkit-csp", "x-frame-options",
                     "x-xss-protection", "x-content-type-options", "x-permitted-cross-domain-policies", "cross-origin-opener-policy",
                     "cross-origin-opener-policy-report-only", "cross-origin-embedder-policy", "cross-origin-embedder-policy-report-only",
                     "cross-origin-resource-policy", "origin-agent-cluster", "permissions-policy", "feature-policy", "document-policy",
                     "referrer-policy", "strict-transport-security", "expect-ct", "public-key-pins", "clear-site-data", "timing-allow-origin",
                     "access-control-expose-headers", "access-control-allow-methods", "access-control-allow-headers",
                     // reporting, and anything that would steer the browser off the proxy
                     "nel", "report-to", "reporting-endpoints", "alt-svc", "link", "speculation-rules", "sourcemap", "x-sourcemap",
                     // hop by hop
                     "keep-alive", "trailer", "upgrade-insecure-requests"});
}

static void tunnel(int fd, Up *u, Worker *w) {
  if (w->inLen > w->inPos && !sendAll(fd, w->io + w->inPos, w->inLen - w->inPos)) return;
  int uf = upFd(*u);
  uint32_t last = millis(), begun = last;
  while (millis() - last < 90000) {
    if (waiting && millis() - begun > 3000) return;  // do not hold up page loads
    bool buffered = u->https && esp_tls_get_bytes_avail(u->t) > 0;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    FD_SET(uf, &r);
    timeval tv = {buffered ? 0 : 1, 0};
    if (select(max(fd, uf) + 1, &r, nullptr, nullptr, &tv) < 0) return;
    if (FD_ISSET(fd, &r)) {
      int n = recv(fd, w->io, IO, 0);
      if (n <= 0 || !upWrite(u, w->io, n)) return;
      last = millis();
    }
    if (buffered || FD_ISSET(uf, &r)) {
      int n = esp_tls_conn_read(u->t, w->io, IO);
      if (tlsWait(n)) continue;
      if (n <= 0 || !sendAll(fd, w->io, n)) return;
      last = millis();
    }
  }
}

static bool staticPath(const std::string &path) {
  std::string p = path.substr(0, path.find_first_of("?#"));
  size_t dot = p.rfind('.'), slash = p.rfind('/');
  if (dot == std::string::npos || dot < slash) return false;
  std::string e = p.substr(dot + 1);
  for (auto &c : e) c = tolower((unsigned char)c);
  return isOneOf(e, {"js", "mjs", "css", "png", "jpg", "jpeg", "gif", "webp", "avif", "svg", "ico", "woff", "woff2", "ttf", "otf", "wasm"});
}

// How long the browser may keep a reply, in seconds; 0 = leave the site's own rule alone.
static long cacheFor(const std::string &path, const std::string &type, const std::string &cc) {
  bool img = type.find("image/") == 0 || type.find("font") != std::string::npos;
  bool code = type.find("text/css") == 0 || type.find("javascript") != std::string::npos || type.find("wasm") != std::string::npos;
  if (!img && !code) return 0;
  bool refused = cc.find("no-store") != std::string::npos || cc.find("no-cache") != std::string::npos || cc.find("private") != std::string::npos;
  // Generated scripts and images (JSONP, captchas) have no file extension and say not to cache.
  if (refused && (!staticPath(path) || path.find('?') != std::string::npos)) return 0;
  long want = img ? 604800 : 86400;
  size_t m = cc.find("max-age=");
  long has = m == std::string::npos ? 0 : atol(cc.c_str() + m + 8);
  return has > want ? has : want;
}

// Sends the response to the client. Returns true if the upstream connection can be reused.
static bool relay(int fd, Worker *w, Up *u, const Req &r, bool &retry, Gunzip *gz) {
  bool over, keep = true, chunked = false, encoded = false, gzipped = false, acac = false, got = false;
  long clen = -1;
  int status = 0;
  BodyKind kind = BODY_RAW;
  std::string out, acao, ctype, cc, expires, vary;
  w->inPos = w->inLen = 0;
  w->stage = "wait headers";
  w->tRead = w->tSend = 0;
  do {
    int n = rdLine(w, u, over);
    if (n < 0) {
      retry = !got;
      return false;
    }
    got = true;
    if (n < 12 || strncmp(w->buf, "HTTP/1.", 7)) {
      errorPage(fd, "502 Bad Gateway", "The site sent a reply the proxy does not understand.");
      return false;
    }
    if (w->buf[7] == '0') keep = false;
    status = atoi(w->buf + 9);
    out = std::string("HTTP/1.1 ") + (w->buf + 9) + "\r\n";
    while ((n = rdLine(w, u, over)) > 0) {
      char *c = over ? nullptr : strchr(w->buf, ':');
      if (!c) continue;
      std::string raw(w->buf, c - w->buf), k = raw, v = trim(c + 1);
      for (auto &x : k) x = tolower((unsigned char)x);
      if (k == "transfer-encoding") chunked = strcasestr(v.c_str(), "chunked") != nullptr;
      else if (k == "content-length") clen = atol(v.c_str());
      else if (k == "connection") keep = keep && !strcasestr(v.c_str(), "close");
      else if (k == "access-control-allow-origin") acao = v;
      else if (k == "access-control-allow-credentials") acac = v == "true";
      else if (!dropHeader(k)) {
        if (k == "content-type") {
          std::string t = v;
          for (auto &x : t) x = tolower((unsigned char)x);
          ctype = t;
          if (t.find("text/html") == 0 || t.find("application/xhtml") == 0) kind = BODY_HTML;
          else if (t.find("text/css") == 0) kind = BODY_CSS;
          else if (t.find("javascript") != std::string::npos || t.find("ecmascript") != std::string::npos) kind = BODY_JS;
        } else if (k == "cache-control") {
          cc = v;
          continue;
        } else if (k == "expires") {
          expires = v;
          continue;
        } else if (k == "vary") {
          vary = v;
          continue;
        } else if (k == "pragma" || k == "age" || k == "accept-ch" || k == "critical-ch" || k == "p3p" || k == "server" || k == "server-timing" || k == "via") {
          continue;
        } else if (k == "content-encoding") {
          encoded = strcasecmp(v.c_str(), "identity") != 0;
          gzipped = !strcasecmp(v.c_str(), "gzip");
          continue;
        }
        else if (k == "location" || k == "refresh" || k == "content-location") v = rewriteString(v);
        else if (k == "set-cookie") v = rewriteSetCookie(v, u->https, u->port);
        if (out.size() + raw.size() + v.size() < 12000) out += raw + ": " + v + "\r\n";
      }
    }
    if (n < 0) return false;
  } while (status >= 100 && status < 200 && status != 101);

  // CORS is opened for everything. Credentialed cross-site reads still need the site's own consent;
  // otherwise any page could read whatever you see on every other site through this proxy.
  bool cred = !r.origin.empty() && acac && acao == unmapUrls(r.origin);
  if (cred) out += "Access-Control-Allow-Origin: " + r.origin + "\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Expose-Headers: *\r\n";
  else out += "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: *\r\n";
  for (auto &x : cc) x = tolower((unsigned char)x);
  long keepFor = r.method == "GET" && status == 200 && !cred ? cacheFor(r.path, ctype, cc) : 0;
  if (keepFor) out += "Cache-Control: public, max-age=" + std::to_string(keepFor) + ", stale-while-revalidate=604800\r\n";
  else {
    // no-store also disables the browser's instant back/forward cache
    size_t ns = cc.find("no-store");
    if (kind == BODY_HTML && ns != std::string::npos) cc = "private, no-cache";
    if (!cc.empty()) out += "Cache-Control: " + cc + "\r\n";
    if (!expires.empty()) out += "Expires: " + expires + "\r\n";
    if (cred) out += "Vary: Origin\r\n";
  }
  out += "Timing-Allow-Origin: *\r\n";
  if (status == 101) {
    keepOpen = false;
    out += "Connection: Upgrade\r\n\r\n";
    if (sendStr(fd, out)) tunnel(fd, u, w);
    return false;
  }
  bool noBody = r.method == "HEAD" || status == 204 || status == 304;
  bool unzip = gzipped && gz && kind != BODY_RAW && status != 206 && !noBody;
  if (encoded && !unzip) {
    kind = BODY_RAW;
    if (!gzipped && !noBody) {
      errorPage(fd, "502 Bad Gateway", "The site used a compression the proxy did not ask for.");
      return false;
    }
    out += "Content-Encoding: gzip\r\n";
  }
  if (status == 206) kind = BODY_RAW;
  bool sized = kind == BODY_RAW && clen >= 0 && !chunked;
  if (sized) out += "Content-Length: " + std::to_string(clen) + "\r\n";
  else if (!noBody) out += "Transfer-Encoding: chunked\r\n";
  out += connHeader();
  out += "\r\n";
  if (!sendStr(fd, out)) return false;
  std::string().swap(out);
  if (noBody) return keep && w->inPos == w->inLen;

  w->cfd = fd;
  Rewriter::Sink sink = [](void *c, const char *d, size_t n) {
    Worker *w = (Worker *)c;
    while (n) {
      size_t t = n < 1424 ? n : 1424;
      int h = sprintf(w->cb, "%x\r\n", (unsigned)t);
      memcpy(w->cb + h, d, t);
      memcpy(w->cb + h + t, "\r\n", 2);
      if (!sendAll(w->cfd, w->cb, h + t + 2)) return false;
      d += t;
      n -= t;
    }
    return true;
  };
  w->rw.begin(kind, sink, w);
  auto pump = [&](long want) -> bool {  // want < 0: until close
    while (want) {
      if (w->inPos == w->inLen) {
        w->stage = "read body";
        uint32_t t0 = millis();
        int g = upRead(u, w->io, IO);
        w->tRead += millis() - t0;
        w->stage = "to client";
        w->bytes = w->bytes + (g > 0 ? g : 0);
        if (g <= 0) return want < 0 && g == 0;
        w->inLen = g;
        w->inPos = 0;
      }
      size_t t = w->inLen - w->inPos;
      if (want > 0 && (long)t > want) t = want;
      uint32_t t1 = millis();
      if (!(sized ? sendAll(fd, w->io + w->inPos, t) : kind == BODY_RAW ? sink(w, w->io + w->inPos, t) : w->rw.feed(w->io + w->inPos, t))) return false;
      w->tSend += millis() - t1;
      w->inPos += t;
      if (want > 0) want -= t;
    }
    return true;
  };
  bool done = false;
  if (unzip) {
    struct Body {
      Worker *w;
      Up *u;
      bool chunked, first, end;
      long left;
    } b = {w, u, chunked, true, false, clen};
    if (chunked) b.left = 0;
    gz->srcCtx = &b;
    gz->src = [](void *c) -> int {
      Body &b = *(Body *)c;
      if (b.end) return -1;
      if (b.left == 0) {
        b.end = true;
        if (!b.chunked) return -1;
        bool over;
        if (!b.first && rdLine(b.w, b.u, over) != 0) return -2;
        b.first = false;
        if (rdLine(b.w, b.u, over) <= 0) return -2;
        b.left = strtol(b.w->buf, nullptr, 16);
        if (b.left <= 0) {
          int n;
          while ((n = rdLine(b.w, b.u, over)) > 0) {}
          return n == 0 ? -1 : -2;
        }
        b.end = false;
      }
      int v = rdByte(b.w, b.u);
      if (v < 0) {
        b.end = true;
        return b.left < 0 ? -1 : -2;
      }
      b.w->bytes = b.w->bytes + 1;
      if (b.left > 0) b.left--;
      return v;
    };
    gz->outCtx = &w->rw;
    gz->out = [](void *c, const char *d, size_t n) { return ((Rewriter *)c)->feed(d, n); };
    w->stage = "unzip";
    done = gz->run();
    // Only a cleanly finished body leaves the connection reusable.
    int tail = done ? gz->src(&b) : -2;
    keep = keep && tail == -1 && (chunked || clen >= 0);
  } else if (chunked) {
    for (;;) {
      if (rdLine(w, u, over) < 0 || over) break;
      char *e;
      long sz = strtol(w->buf, &e, 16);
      if (e == w->buf || sz < 0) break;
      if (!sz) {
        int n;
        while ((n = rdLine(w, u, over)) > 0) {}
        done = n == 0;
        break;
      }
      if (!pump(sz) || rdLine(w, u, over) != 0) break;
    }
  } else {
    done = pump(clen);
    if (clen < 0) keep = false;
  }
  if (kind != BODY_RAW) done = w->rw.finish() && done;
  if (done && !sized) done = sendAll(fd, "0\r\n\r\n", 5);
  if (lanTest) Serial.printf("timing: %u bytes, read %u ms, rewrite+send %u ms\n", (unsigned)w->bytes, (unsigned)w->tRead, (unsigned)w->tSend);
  if (!done && lanTest) Serial.printf("E (cut) %s after %u bytes, errno %d, %u ms", w->stage, (unsigned)w->bytes, errno, (unsigned)(millis() - w->since)), Serial.println();
  if (!done) keepOpen = false;
  return done && keep && w->inPos == w->inLen;
}

// Telemetry and ads compete with the page for the one or two upstream connections.
static bool isTracker(const std::string &h, const std::string &p) {
  for (auto d : {"doubleclick.net", "google-analytics.com", "googletagmanager.com", "googlesyndication.com", "googleadservices.com", "scorecardresearch.com"}) {
    size_t n = strlen(d);
    if (h.size() >= n && h.compare(h.size() - n, n, d) == 0 && (h.size() == n || h[h.size() - n - 1] == '.')) return true;
  }
  if (h == "play.google.com" && p.compare(0, 4, "/log") == 0) return true;
  // YouTube's hidden "is the visitor already signed in" frame: a 1.2 MB page nobody sees.
  bool probe = p.compare(0, 13, "/generate_204") == 0;
  if (probe && (h.find("googlevideo.com") != std::string::npos || h.find("ytimg.com") != std::string::npos)) return true;
  if (h.find("google.") == std::string::npos && h.find("youtube.") == std::string::npos) return false;
  for (auto x : {"/gen_204", "/generate_204", "/api/stats/", "/youtubei/v1/log_event", "/pagead/", "/ptracking", "/csi_204", "/error_204"})
    if (p.compare(0, strlen(x), x) == 0) return true;
  return false;
}

static void proxy(int fd, Worker *w, const Req &r, const std::string &first, const std::string &host, int port, bool https) {
  if (r.method == "OPTIONS" && !r.acrm.empty()) {
    sendStr(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n" + std::string(connHeader()) + "Access-Control-Allow-Origin: " + r.origin +
                    "\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Methods: " + r.acrm + "\r\nAccess-Control-Allow-Headers: " + r.acrh +
                    "\r\nAccess-Control-Max-Age: 600\r\nAccess-Control-Allow-Private-Network: true\r\nVary: Origin\r\n\r\n");
    return;
  }
  if (isTracker(host, r.path)) {
    std::string c = r.origin.empty() ? "" : "Access-Control-Allow-Origin: " + r.origin + "\r\nAccess-Control-Allow-Credentials: true\r\n";
    sendStr(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n" + std::string(connHeader()) + c + "\r\n");
    return;
  }
  // Reload of a static file the browser already has: say "unchanged" without asking the site.
  // A forced reload (Ctrl+F5) sends no-cache and still goes through.
  if (r.method == "GET" && r.revalidate && !r.hard && staticPath(r.path)) {
    sendStr(fd, std::string("HTTP/1.1 304 Not Modified\r\nCache-Control: public, max-age=86400\r\nAccess-Control-Allow-Origin: *\r\n") + connHeader() + "\r\n");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return errorPage(fd, "503 Service Unavailable", "The proxy is not connected to the internet.");
  if (r.chunked) return reply(fd, "411 Length Required", "text/plain", "Chunked uploads are not supported");
  bool ws = !strcasecmp(r.upgrade.c_str(), "websocket");
  if (ws) {
    xSemaphoreTake(poolMx, portMAX_DELAY);
    bool full = wsCount >= 1;
    if (!full) wsCount = wsCount + 1;
    xSemaphoreGive(poolMx);
    if (full) return reply(fd, "503 Service Unavailable", "text/plain", "WebSocket limit reached");
  }
  std::string head = r.method + " " + unmapUrls(r.path) + " HTTP/1.1\r\nHost: " + realOrigin(host, port, https).substr(https ? 8 : 7) + "\r\n" + r.fwd;
  if (!r.origin.empty()) head += "Origin: " + unmapUrls(r.origin) + "\r\n";
  if (!r.referer.empty() && r.referer.find("//proxy.esp32/") == std::string::npos) head += "Referer: " + unmapUrls(r.referer) + "\r\n";
  if (r.bodyLen || isOneOf(r.method, {"POST", "PUT", "PATCH"})) head += "Content-Length: " + std::to_string(r.bodyLen) + "\r\n";
  // Pages and stylesheets are unpacked here to be rewritten; if there is no memory for that, they are
  // requested uncompressed. Everything else is passed through compressed (about 4x smaller), and the
  // injected hook handles URLs inside scripts.
  head += "Save-Data: on\r\n";
  head += ws ? "Connection: Upgrade\r\nUpgrade: websocket\r\n" : "Connection: keep-alive\r\n";

  // Only idempotent requests may use a pooled connection, since only they can be safely replayed.
  bool safe = !ws && !r.bodyLen && (r.method == "GET" || r.method == "HEAD");
  for (int attempt = 0; attempt < 2; attempt++) {
    bool reused = false, retry = false, keep = false;
    const char *err = "";
    w->stage = "connect";
    w->since = millis();
    w->bytes = 0;
    Up *u = upAcquire(host, port, https, attempt > 0 || !safe, reused, err);
    if (!u) {
      errorPage(fd, "502 Bad Gateway", err);
      break;
    }
    w->stage = "send request";
    Gunzip *gz = r.gzip && r.html && !ws ? gunzipNew() : nullptr;
    const char *enc = ws ? "\r\n" : r.gzip && (!r.html || gz) ? "Accept-Encoding: gzip\r\n\r\n" : "Accept-Encoding: identity\r\n\r\n";
    bool ok = upWrite(u, head.data(), head.size()) && upWrite(u, enc, strlen(enc));
    if (ok && r.bodyLen) {
      long left = r.bodyLen - (long)first.size();
      ok = left >= 0 && upWrite(u, first.data(), first.size());
      while (ok && left > 0) {
        int n = recv(fd, w->io, left < IO ? left : IO, 0);
        ok = n > 0 && upWrite(u, w->io, n);
        left -= n;
      }
      if (ok && left == 0 && !r.close) keepOpen = true;
    }
    try {
      if (ok) keep = relay(fd, w, u, r, retry, gz);
      else retry = true;
    } catch (...) {
      keepOpen = false;
      Serial.println("out of memory, request dropped");
    }
    gunzipFree(gz);
    upRelease(u, keep);
    if (!retry) break;
    if (!reused) {
      errorPage(fd, "502 Bad Gateway", "The site closed the connection without answering.");
      break;
    }
  }
  if (ws) {
    xSemaphoreTake(poolMx, portMAX_DELAY);
    wsCount = wsCount - 1;
    xSemaphoreGive(poolMx);
  }
}

static void handle(int fd, Worker *w) {
  keepOpen = false;
  size_t n = 0;
  char *end = nullptr;
  while (!end) {
    if (n >= BUF - 1) return reply(fd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
    int g = recv(fd, w->buf + n, BUF - 1 - n, 0);
    if (g <= 0) return;
    n += g;
    w->buf[n] = 0;
    end = strstr(w->buf, "\r\n\r\n");
  }
  std::string first(end + 4, w->buf + n - (end + 4));
  end[2] = 0;
  Req r;
  char *line = w->buf, *eol = strstr(line, "\r\n");
  *eol = 0;
  char *sp = strchr(line, ' '), *sp2 = sp ? strchr(sp + 1, ' ') : nullptr;
  if (!sp2 || sp[1] != '/') return reply(fd, "400 Bad Request", "text/plain", "Bad request");
  r.method.assign(line, sp - line);
  r.path.assign(sp + 1, sp2 - sp - 1);
  for (line = eol + 2; *line; line = eol + 2) {
    eol = strstr(line, "\r\n");
    if (!eol) break;
    *eol = 0;
    char *c = strchr(line, ':');
    if (!c) continue;
    std::string raw(line, c - line), k = raw, v = trim(c + 1);
    for (auto &x : k) x = tolower((unsigned char)x);
    if (k == "host") r.host = v;
    else if (k == "origin") r.origin = v;
    else if (k == "referer") r.referer = v;
    else if (k == "upgrade") r.upgrade = v;
    else if (k == "content-length") r.bodyLen = atol(v.c_str());
    else if (k == "transfer-encoding") r.chunked = true;
    else if (k == "access-control-request-method") r.acrm = v;
    else if (k == "access-control-request-headers") r.acrh = v;
    else if (k == "accept-encoding") r.gzip = v.find("gzip") != std::string::npos;
    else if (k == "cookie") r.fwd += raw + ": " + unmapCookies(v) + "\r\n";
    else if (k == "connection") r.close = strcasestr(v.c_str(), "close") != nullptr;
    else if (!isOneOf(k, { "keep-alive", "te", "trailer", "expect", "save-data", "proxy-connection", "access-control-request-private-network"}))
      r.fwd += raw + ": " + v + "\r\n";
    if (k == "if-none-match") r.inm = v;
    if (k == "range") r.range = v;
    if (k == "content-type") r.ctype = v;
    if (k == "if-none-match" || k == "if-modified-since") r.revalidate = true;
    if (k == "cache-control" || k == "pragma") r.hard = r.hard || v.find("no-cache") != std::string::npos;
    if (k == "accept") r.html = v.find("text/html") != std::string::npos || (v.find("text/css") != std::string::npos && r.host.find("www.youtube.com") != 0);
  }
  // A body not fully read, or a HEAD, would leave the two ends out of step.
  keepOpen = !strcmp(sp2 + 1, "HTTP/1.1") && !r.close && !r.chunked && r.method != "HEAD" && r.bodyLen == (long)first.size();
  if (r.bodyLen < 0) return reply(fd, "400 Bad Request", "text/plain", "Bad request");
  for (auto &c : r.host) c = tolower((unsigned char)c);
  std::string host;
  int port;
  bool https;
  if (r.host == "proxy.esp32") {
    if (r.bodyLen > 1024) return reply(fd, "413 Content Too Large", "text/plain", "Too large");
    while ((long)first.size() < r.bodyLen) {
      int g = recv(fd, w->io, IO, 0);
      if (g <= 0) return;
      first.append(w->io, g);
    }
    portal(fd, r, first);
  } else if (unmapHost(r.host, host, port, https)) {
    if (r.path.compare(0, 13, "/__px/hook.js") == 0) {
      std::string h = "HTTP/1.1 200 OK\r\nContent-Type: text/javascript; charset=utf-8\r\nContent-Length: " + std::to_string(r.gzip ? sizeof(HOOK_GZ) : strlen(HOOK_JS)) +
                      "\r\nCache-Control: public, max-age=604800, immutable\r\n" + (r.gzip ? "Content-Encoding: gzip\r\n" : "") + connHeader() + "\r\n";
      const char *body = r.gzip ? (const char *)HOOK_GZ : HOOK_JS;
      size_t len = r.gzip ? sizeof(HOOK_GZ) : strlen(HOOK_JS);
      if (len + h.size() < 2800) sendStr(fd, h.append(body, len));  // one write, so no waiting between packets
      else if (sendStr(fd, h)) sendAll(fd, body, len);
    }
    else if (lanTest && r.path == "/__px/speed") {
      keepOpen = false;
      sendStr(fd, "HTTP/1.1 200 OK\r\nContent-Length: 300000\r\nConnection: close\r\n\r\n");
      memset(w->io, 'x', IO);
      for (int left = 300000; left > 0; left -= IO)
        if (!sendAll(fd, w->io, min(left, IO))) break;
    } else proxy(fd, w, r, first, host, port, https);
  } else captive(fd, w, r, first);
}

static TaskHandle_t taskH[WORKERS + 1];
static void workerTask(void *n) {
  Worker *w = workers[(int)n] = new Worker;
  int fd;
  for (;;) {
    xQueueReceive(jobs, &fd, portMAX_DELAY);
    timeval tv = {10, 0};
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 60;  // browsers pause reading a page while its scripts load
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    bool more;
    do {
      try {
        handle(fd, w);
      } catch (...) {
        keepOpen = false;
        Serial.println("out of memory, request dropped");
      }
      w->stage = "idle";
      more = false;
      if (keepOpen && !uxQueueMessagesWaiting(jobs)) {
        // The next request usually follows at once; take it directly.
        fd_set f;
        FD_ZERO(&f);
        FD_SET(fd, &f);
        timeval q = {0, 25000};
        more = select(fd + 1, &f, nullptr, nullptr, &q) > 0 && heap_caps_get_free_size(MALLOC_CAP_8BIT) > 16000;
      }
    } while (more);
    if (keepOpen && xQueueSend(idleQ, &fd, 0) == pdTRUE) continue;
    if (!keepOpen) {
      // Unread input would turn the close into a reset before the browser has read the reply.
      timeval q = {0, 60000};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &q, sizeof(q));
      for (int i = 0; i < 20 && recv(fd, w->io, IO, MSG_DONTWAIT) > 0; i++) {}
    }
    close(fd);
  }
}

// ---------- DNS: every name is this device ----------
static void dnsPoll() {
  uint8_t p[300];
  sockaddr_in from;
  socklen_t fl = sizeof(from);
  int n = recvfrom(dnsFd, p, 260, MSG_DONTWAIT, (sockaddr *)&from, &fl);
  if (n < 17 || (p[2] & 0xF8) || p[4] || p[5] != 1) return;
  int q = 12;
  while (q < n && p[q]) {
    if (p[q] > 63) return;
    q += p[q] + 1;
  }
  if (q + 5 > n) return;
  int type = p[q + 1] << 8 | p[q + 2];
  n = q + 5;
  p[2] = 0x84 | (p[2] & 1);
  p[3] = 0x80;
  memset(p + 6, 0, 6);
  if (type == 1) {
    p[7] = 1;
    const uint8_t a[16] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
    memcpy(p + n, a, 16);
    n += 16;
  }
  sendto(dnsFd, p, n, 0, (sockaddr *)&from, fl);
}

static void mdnsPoll() {
  uint8_t p[512];
  sockaddr_in from;
  socklen_t fl = sizeof(from);
  int n = recvfrom(mdnsFd, p, sizeof(p), MSG_DONTWAIT, (sockaddr *)&from, &fl);
  if (n < 17 || (p[2] & 0x80)) return;
  int q = 12, count = p[4] << 8 | p[5];
  bool mine = false;
  while (count-- > 0) {
    std::string name;
    while (q < n && p[q] && p[q] < 64) {
      if (q + 1 + p[q] > n) return;
      if (!name.empty()) name += '.';
      name.append((char *)p + q + 1, p[q]);
      q += p[q] + 1;
    }
    if (q >= n) return;
    q += p[q] ? 2 : 1;
    if (q + 4 > n) return;
    int type = p[q] << 8 | p[q + 1];
    q += 4;
    mine |= (type == 1 || type == 255) && !strcasecmp(name.c_str(), localName.c_str());
  }
  if (!mine) return;
  std::string o("\0\0\x84\0\0\0\0\1\0\0\0\0", 12);
  size_t a = 0;
  while (a < localName.size()) {
    size_t e = localName.find('.', a);
    if (e == std::string::npos) e = localName.size();
    o += (char)(e - a);
    o += localName.substr(a, e - a);
    a = e + 1;
  }
  o += std::string("\0\0\1\x80\1\0\0\0\x78\0\4\xC0\xA8\4\1", 15);
  if (ntohs(from.sin_port) == 5353) from.sin_addr.s_addr = inet_addr("224.0.0.251");
  else memcpy(&o[0], p, 2);
  sendto(mdnsFd, o.data(), o.size(), 0, (sockaddr *)&from, sizeof(from));
}

// Hands a connection to a worker only once it has data: browsers open spare connections
// that would otherwise block a worker doing nothing.
static void acceptTask(void *) {
  const int MAXW = 4;  // 16 sockets in total
  int pending[MAXW], pn = 0;
  uint32_t since[MAXW];
  bool idle[MAXW];  // has already carried a request
  auto removeAt = [&](int i) {
    pn--;
    memmove(pending + i, pending + i + 1, (pn - i) * sizeof(int));
    memmove(since + i, since + i + 1, (pn - i) * sizeof(uint32_t));
    memmove(idle + i, idle + i + 1, (pn - i) * sizeof(bool));
  };
  auto add = [&](int fd, bool wasUsed) {
    pending[pn] = fd;
    since[pn] = millis();
    idle[pn++] = wasUsed;
  };
  for (;;) {
    int back;
    while (xQueueReceive(idleQ, &back, 0) == pdTRUE) {
      if (pn < MAXW) add(back, true);
      else close(back);
    }
    int victim = -1;  // oldest idle connection, given up when a new one needs the place
    for (int i = 0; i < pn && victim < 0; i++)
      if (idle[i]) victim = i;
    // Under about 16 KB the network stack itself starts failing: let browsers wait instead.
    bool space = uxQueueSpacesAvailable(jobs) > 0 && heap_caps_get_free_size(MALLOC_CAP_8BIT) > 16000;
    fd_set r;
    FD_ZERO(&r);
    int mx = listenFd;
    if (dnsFd >= 0) {
      FD_SET(dnsFd, &r);
      mx = max(mx, dnsFd);
    }
    if (mdnsFd >= 0) {
      FD_SET(mdnsFd, &r);
      mx = max(mx, mdnsFd);
    }
    if (pn < MAXW || victim >= 0) {
      FD_SET(listenFd, &r);
      mx = max(mx, listenFd);
    }
    if (space)
      for (int i = 0; i < pn; i++) {
        FD_SET(pending[i], &r);
        mx = max(mx, pending[i]);
      }
    timeval tv = {0, 8000};
    if (select(mx + 1, &r, nullptr, nullptr, &tv) < 0) {
      delay(10);
      continue;
    }
    if (dnsFd >= 0 && FD_ISSET(dnsFd, &r)) dnsPoll();
    if (mdnsFd >= 0 && FD_ISSET(mdnsFd, &r)) mdnsPoll();
    for (int i = 0; i < pn;) {
      bool data = space && FD_ISSET(pending[i], &r);
      if (data && xQueueSend(jobs, &pending[i], 0) == pdTRUE) removeAt(i);
      else if (space && !data && millis() - since[i] > (idle[i] ? 15000 : 4000)) {
        close(pending[i]);
        removeAt(i);
      } else i++;
    }
    if (FD_ISSET(listenFd, &r)) {
      if (pn == MAXW)
        for (int i = 0; i < pn; i++)
          if (idle[i]) {
            close(pending[i]);
            removeAt(i);
            break;
          }
      if (pn == MAXW) continue;
      int c = accept(listenFd, nullptr, nullptr);
      if (c < 0) {
        delay(20);
        continue;
      }
      sockaddr_in me;
      socklen_t l = sizeof(me);
      // Serve the access point only; never be an open proxy on the upstream network.
      if (getsockname(c, (sockaddr *)&me, &l) || (!lanTest && me.sin_addr.s_addr != (uint32_t)AP_IP)) {
        close(c);
        continue;
      }
      add(c, false);
    }
  }
}

// ---------- serial console ----------
static void selfTest() {
  int bad = 0;
  auto chk = [&](const std::string &got, const std::string &want) {
    if (got == want) return;
    bad++;
    Serial.printf("FAIL\n got  %s\n want %s\n", got.c_str(), want.c_str());
  };
  auto run = [](BodyKind k, const std::string &s, size_t step) {
    std::string out;
    auto *r = new Rewriter;
    r->begin(k, appendSink, &out);
    for (size_t i = 0; i < s.size(); i += step) r->feed(s.data() + i, min(step, s.size() - i));
    r->finish();
    delete r;
    return out;
  };
  const std::string HK = "<script src=\"/__px/hook.js?7\"></script>", pad(1500, 'x');
  for (size_t step : {1, 7, 500, 4000}) {
    chk(run(BODY_HTML, pad + "<html><head lang=\"a\"><a href=\"https://a.com/x\"><script src=//b.org/y integrity=\"z\">", step),
        pad + "<html><head lang=\"a\">" + HK + "<a href=\"http://a.com.esp32/x\"><script src=http://b.org.esp32/y data-px-integrity=\"z\">");
    chk(run(BODY_HTML, "<header><script>1</script>", step), "<header>" + HK + "<script>1</script>");
    chk(run(BODY_HTML, "<head><meta http-equiv=\"Content-Security-Policy\" content=\"x\">", step),
        "<head>" + HK + "<meta http-equiv=\"x-removed-policy\" content=\"x\">");
    chk(run(BODY_CSS, "a{b:url(//c.net/i.png)}@import 'http://d.io:8080/z'", step), "a{b:url(http://c.net.esp32/i.png)}@import 'http://d.io.h8080.esp32/z'");
    chk(run(BODY_JS, "a='https:\\/\\/e.com\\/q';// http://www.w3.org/2000/svg\nb=`https://${h}`;c=\"https://f.com:8443\"", step),
        "a='http:\\/\\/e.com.esp32\\/q';// http://www.w3.org/2000/svg\nb=`https://${h}`;c=\"http://f.com.p8443.esp32\"");
  }
  chk(rewriteString("https://x.com/a"), "http://x.com.esp32/a");
  chk(rewriteString("//x.com/a"), "http://x.com.esp32/a");
  chk(rewriteString("/rel"), "/rel");
  chk(rewriteString("0;url=https://x.com/a"), "0;url=http://x.com.esp32/a");
  chk(unmapUrls("http://x.com.esp32/a?u=http%3A%2F%2Fy.com.h.esp32%2F"), "https://x.com/a?u=http%3A%2F%2Fy.com%2F");
  chk(unmapUrls("http://x.com.p8443.esp32"), "https://x.com:8443");
  chk(rewriteSetCookie("__Host-a=b; Path=/; Secure; SameSite=None; HttpOnly; Domain=.x.com", true, 443), "xHost-a=b; Path=/; HttpOnly; Domain=x.com.esp32");
  chk(unmapCookies("q=1; xHost-a=b"), "q=1; __Host-a=b");
  Serial.printf("selftest: %s (%d failures)\n", bad ? "FAILED" : "OK", bad);
}

static const char *resetName(int r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (out of memory?)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog (stuck)";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (weak USB power)";
    default: return "unknown";
  }
}
static void status() {
  Serial.printf("AP \"%s\" password \"%s\" | internet: %s | rssi=%d | heap=%u max=%u min=%u\n", apSsid.c_str(), apPass.c_str(),
                staSsid.empty() ? "not configured" : WiFi.status() == WL_CONNECTED ? "connected" : "not connected", (int)WiFi.RSSI(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  Serial.printf("  last reset: %s\n", resetName(bootReason));
  Serial.printf("  stack left: %u %u accept %u | heap intact: %d\n", (unsigned)uxTaskGetStackHighWaterMark(taskH[0]), (unsigned)uxTaskGetStackHighWaterMark(taskH[1]), (unsigned)uxTaskGetStackHighWaterMark(taskH[2]), heap_caps_check_integrity_all(false));
  wifi_ps_type_t ps;
  esp_wifi_get_ps(&ps);
  uint8_t ch;
  wifi_second_chan_t c2;
  esp_wifi_get_channel(&ch, &c2);
  Serial.printf("  power save %d, channel %d, AP clients %d\n", ps, ch, WiFi.softAPgetStationNum());
  for (int i = 0; i < WORKERS; i++)
    if (workers[i] && strcmp(workers[i]->stage, "idle")) Serial.printf("  worker %d: %s, %u bytes, %u ms\n", i, workers[i]->stage, (unsigned)workers[i]->bytes, (unsigned)(millis() - workers[i]->since));
  Serial.println("Commands: setap=Name;password  setwifi=Name;password  reset  factory  mac[=AA:BB:CC:DD:EE:FF]  lan  scan  selftest  status");
}

static std::string mdnsHost() {
  std::string h = apSsid;
  for (auto &c : h) c = strchr(" ./_", c) ? '-' : tolower((unsigned char)c);
  return h;
}
// Starts the access point in response to a setap= command, printing the exact lines the Flipper reads.
// (The original ran WiFi.disconnect() first; we keep the home-Wi-Fi uplink up so the proxy still works.)
static void startAp(std::string name, std::string pass) {
  apSsid = name.substr(0, 31);
  apPass = pass.substr(0, 63);
  Serial.printf("AP name: %s\n", apSsid.c_str());
  Serial.printf("AP password: %s\n", apPass.empty() ? "(none)" : apPass.c_str());
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), AP_IP);
  WiFi.softAP(apSsid.c_str(), apPass.empty() ? nullptr : apPass.c_str(), 1, apHidden ? 1 : 0, 4);
  apStarted = true;
  prefs.putString("ap", apSsid.c_str());
  prefs.putString("appw", apPass.c_str());
  prefs.putBool("apup", true);
  Serial.print("AP started: ");
  Serial.println(apSsid.c_str());
  Serial.print("IP: ");
  sockaddr_in a = {};
  int one = 1;
  a.sin_family = AF_INET;
  if (dnsFd < 0) {
    dnsFd = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(53);
    a.sin_addr.s_addr = (uint32_t)AP_IP;
    if (bind(dnsFd, (sockaddr *)&a, sizeof(a))) {
      close(dnsFd);
      dnsFd = -1;
    }
  }
  if (mdnsFd < 0) {
    localName = mdnsHost() + ".local";
    mdnsFd = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(5353);
    a.sin_addr.s_addr = INADDR_ANY;
    ip_mreq m = {};
    m.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    m.imr_interface.s_addr = (uint32_t)AP_IP;
    in_addr ifc = m.imr_interface;
    uint8_t ttl = 255;
    setsockopt(mdnsFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(mdnsFd, (sockaddr *)&a, sizeof(a)) || setsockopt(mdnsFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m))) {
      close(mdnsFd);
      mdnsFd = -1;
    } else {
      setsockopt(mdnsFd, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof(ifc));
      setsockopt(mdnsFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
  }
  Serial.printf("Website: http://%s.local\n", mdnsHost().c_str());
  Serial.println(AP_IP);
}

#include "fap.h"

static void serialCmd(const std::string &s) {
  if (fapCmd(s)) return;  // Flipper control app
  size_t eq = s.find('='), sc = s.find(';');
  std::string k = s.substr(0, eq), a, b;
  if (eq != std::string::npos) {
    a = s.substr(eq + 1, sc == std::string::npos ? sc : sc - eq - 1);
    if (sc != std::string::npos) b = s.substr(sc + 1);
  }
  if (k == "selftest") return selfTest();
  if (k == "psram") return (void)Serial.printf("psram found=%d size=%u\n", psramFound(), (unsigned)ESP.getPsramSize());
  if (k == "bench") {
    std::string t;
    while (t.size() < 4000) t += "<div class=\"item\" id=\"x1\"><a href=\"https://example.com/a\">some ordinary text here</a></div>\n";
    auto *rw = new Rewriter;
    rw->begin(BODY_HTML, [](void *, const char *, size_t) { return true; }, nullptr);
    uint32_t t0 = millis();
    for (int i = 0; i < 50; i++) rw->feed(t.data(), t.size());
    rw->finish();
    t0 = millis() - t0;
    delete rw;
    return (void)Serial.printf("bench: rewrite %u KB/s\n", (unsigned)(50 * t.size() / (t0 + 1)));
  }
  if (k == "scan") {  // how crowded each 2.4 GHz channel is
    int n = WiFi.scanNetworks(false, true);
    for (int c = 1; c <= 13; c++) {
      int count = 0, best = -100;
      for (int i = 0; i < n; i++)
        if (WiFi.channel(i) == c) {
          count++;
          if (WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
        }
      if (count) Serial.printf("channel %d: %d networks, strongest %d dBm\n", c, count, best);
    }
    WiFi.scanDelete();
    return;
  }
  if (k == "mac") {
    if (a.empty()) {
      uint8_t m[6];
      esp_wifi_get_mac(WIFI_IF_STA, m);
      Serial.printf("STA MAC: %s%s\n", macStr(m).c_str(), prefs.getBytesLength("mac") == 6 ? " (custom)" : " (default)");
      return;
    }
    std::string out, err = setMac(a, out);
    Serial.println(err.empty() ? ("MAC set to " + out).c_str() : ("MAC error: " + err).c_str());
    return;
  }
  if (k == "crashtest") {  // undocumented: forces a panic to test auto-recovery
    volatile int *z = nullptr;
    *z = 1;
    return;
  }
  if (k == "restart" || k == "reset") {
    Serial.println("Resetting...");
    delay(50);
    ESP.restart();
  }
  if (k == "tx") return (void)Serial.printf("tx power %d (quarter dBm): %d\n", atoi(a.c_str()), esp_wifi_set_max_tx_power(atoi(a.c_str())));
  if (k == "lan") {
    lanTest = true;
    Serial.printf("lan test on until restart: %s\n", WiFi.localIP().toString().c_str());
    return;
  }
  if (k == "setap") {
    if (apStarted) {
      Serial.println("AP already started, ignoring command.");
      return;
    }
    startAp(a, b);
    return;
  }
  if (k == "setwifi" && !a.empty() && a.size() <= 32 && b.size() <= 63) saveSta(a, b);
  else if (k == "factory") prefs.clear();
  else return status();
  Serial.println("Saved, restarting");
  delay(200);
  ESP.restart();
}

SET_LOOP_TASK_STACK_SIZE(6144);
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  prefs.begin("pxproxy");
  Serial.println("\nWaiting for AP name via serial...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  apSsid = prefs.getString("ap", "ESP32 Proxy").c_str();
  apPass = prefs.getString("appw", "").c_str();
  staSsid = prefs.getString("sta", "").c_str();
  staPass = prefs.getString("stapw", "").c_str();
  apHidden = prefs.getBool("aphidden", false);
  homePage = prefs.getString("home", "/index.html").c_str();
  WiFi.setSleep(false);
  uint8_t cmac[6];
  if (prefs.getBytesLength("mac") == 6 && prefs.getBytes("mac", cmac, 6) == 6) esp_wifi_set_mac(WIFI_IF_STA, cmac);
  if (!staSsid.empty()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(staSsid.c_str(), staPass.empty() ? nullptr : staPass.c_str());
  }
  gunzip.win = (volatile uint32_t *)heap_caps_malloc(32768, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
  if (!gunzip.win) Serial.println("no window memory: pages will be fetched uncompressed");
#ifdef SD_TEST
  hasSD = LittleFS.begin(true, SD_ROOT, 4, "spiffs");  // flash stands in for the card
#elif defined(NO_SD)
  hasSD = false;  // proxy-only build (e.g. the Wi-Fi Devboard has no SD): portal serves from flash
#else
  hasSD = sdMount();
#endif
  poolMx = xSemaphoreCreateMutex();
  connMx = xSemaphoreCreateMutex();
  jobs = xQueueCreate(2, sizeof(int));
  idleQ = xQueueCreate(4, sizeof(int));
  sockaddr_in a = {};
  int one = 1;
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons(80);
  listenFd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(listenFd, (sockaddr *)&a, sizeof(a)) || listen(listenFd, 10)) Serial.println("listen failed");
  Serial.println("HTTP server started");
  if (hasSD) Serial.println("SD Card initialized.");
  for (int i = 0; i < WORKERS; i++)
    if (xTaskCreatePinnedToCore(workerTask, "px", 8192, (void *)i, 3, &taskH[i], i & 1) != pdPASS) Serial.println("worker failed");
  xTaskCreatePinnedToCore(acceptTask, "accept", 3584, nullptr, 4, &taskH[WORKERS], 1);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(TX_POWER);
  bootReason = esp_reset_reason();
  // A crash or brownout must not leave the AP down until a human re-sends setap. A clean power-on or a
  // serial reset still waits for setap, so the Flipper flow is unchanged.
  bool crashed = bootReason == ESP_RST_PANIC || bootReason == ESP_RST_INT_WDT || bootReason == ESP_RST_TASK_WDT || bootReason == ESP_RST_WDT || bootReason == ESP_RST_BROWNOUT;
  if (crashed && prefs.getBool("apup", false)) {
    Serial.printf("Recovering after reset: %s\n", resetName(bootReason));
    startAp(prefs.getString("ap", "ESP32 Proxy").c_str(), prefs.getString("appw", "").c_str());
  }
}

void loop() {
  static std::string in;
  static uint32_t down = 0, lost = 0;
  static bool up = false;
  if (up != (WiFi.status() == WL_CONNECTED)) {
    up = !up;
    if (up) {
      esp_wifi_set_ps(WIFI_PS_NONE);
      esp_wifi_set_max_tx_power(TX_POWER);
    }
  }
  // Memory guardian: when the largest free block gets dangerously small, drop IDLE upstream connections
  // (each frees ~36 KB) so the Wi-Fi driver always has buffers to keep clients associated. Active requests
  // are untouched; this only costs a little keep-alive speed, and only under real pressure.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 9000) {
    xSemaphoreTake(poolMx, portMAX_DELAY);
    for (auto &p : pool)
      if (p.t && !p.busy) upClose(p);
    xSemaphoreGive(poolMx);
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!in.empty()) serialCmd(in);
      in.clear();
    } else if (in.size() < 160) in += c;
  }
  if (digitalRead(0) == LOW) {
    if (!down) down = millis();
    if (millis() - down > 5000) serialCmd("factory");
  } else down = 0;
  if (!staSsid.empty() && WiFi.status() != WL_CONNECTED) {
    if (millis() - lost > 20000) {
      lost = millis();
      WiFi.reconnect();
    }
  } else lost = millis();
  delay(20);
}
