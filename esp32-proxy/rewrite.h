// URL mapping and streaming body rewriting.
//
//   https://example.com/a       <-> http://example.com.esp32/a
//   http://example.com/a        <-> http://example.com.h.esp32/a
//   https://example.com:8443/a  <-> http://example.com.p8443.esp32/a
//   http://example.com:8080/a   <-> http://example.com.h8080.esp32/a
//
// One hostname per origin means relative URLs, cookies and localStorage all work natively.
#pragma once
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static inline bool ieq(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}
static inline bool hostChar(char c) { return isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_'; }
static inline bool endsPx(const char *h, size_t n) { return n > 6 && ieq(h + n - 6, ".esp32", 6); }

// A dotted name with an alphabetic (or punycode) TLD, or a dotted IPv4 address.
static bool validHost(const char *h, size_t n) {
  if (n < 3 || n > 230 || h[0] == '.' || h[0] == '-' || h[n - 1] == '.' || h[n - 1] == '-') return false;
  int dots = 0;
  size_t last = 0;
  bool numeric = true;
  for (size_t i = 0; i < n; i++) {
    if (!hostChar(h[i])) return false;
    if (h[i] == '.') {
      if (h[i + 1] == '.') return false;
      dots++;
      last = i + 1;
    } else if (!isdigit((unsigned char)h[i])) numeric = false;
  }
  if (!dots) return false;
  if (numeric) return dots == 3;
  return n - last >= 2 && isalpha((unsigned char)h[last]);
}

static std::string mapHost(bool https, const std::string &host, int port) {
  std::string r = host;
  if (https) {
    if (port && port != 443) r += ".p" + std::to_string(port);
  } else {
    r += ".h";
    if (port && port != 80) r += std::to_string(port);
  }
  return r + ".esp32";
}

static bool unmapHost(const std::string &in, std::string &host, int &port, bool &https) {
  std::string h = in.substr(0, in.find(':'));
  for (auto &c : h) c = tolower((unsigned char)c);
  if (!endsPx(h.c_str(), h.size())) return false;
  h.resize(h.size() - 6);
  https = true;
  port = 443;
  size_t dot = h.rfind('.');
  if (dot != std::string::npos) {
    std::string l = h.substr(dot + 1);
    bool digits = l.size() > 1 && l.size() <= 6;
    for (size_t i = 1; i < l.size(); i++) digits = digits && isdigit((unsigned char)l[i]);
    if (l == "h") {
      https = false;
      port = 80;
      h.resize(dot);
    } else if (digits && (l[0] == 'h' || l[0] == 'p')) {
      https = l[0] == 'p';
      port = atoi(l.c_str() + 1);
      if (port < 1 || port > 65535) return false;
      h.resize(dot);
    }
  }
  host = h;
  return validHost(h.c_str(), h.size());
}

static std::string realOrigin(const std::string &host, int port, bool https) {
  std::string r = (https ? "https://" : "http://") + host;
  if (port != (https ? 443 : 80)) r += ":" + std::to_string(port);
  return r;
}

// Turn proxied URLs inside a string (Referer, Origin, query values) back into real ones.
static std::string unmapUrls(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    size_t p = 0;
    bool enc = false;
    if (s.size() - i > 7 && ieq(&s[i], "http://", 7)) p = 7;
    else if (s.size() - i > 13 && ieq(&s[i], "http%3A%2F%2F", 13)) p = 13, enc = true;
    if (p) {
      size_t e = i + p;
      while (e < s.size() && hostChar(s[e])) e++;
      std::string host;
      int port;
      bool https;
      if (unmapHost(s.substr(i + p, e - i - p), host, port, https)) {
        std::string o = realOrigin(host, port, https);
        if (!enc) out += o;
        else
          for (char c : o) {
            if (c == ':') out += "%3A";
            else if (c == '/') out += "%2F";
            else out += c;
          }
        i = e;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

enum BodyKind { BODY_RAW, BODY_HTML, BODY_CSS, BODY_JS };

class Rewriter {
 public:
  typedef bool (*Sink)(void *, const char *, size_t);

  void begin(BodyKind k, Sink s, void *c) {
    kind = k;
    sink = s;
    ctx = c;
    n = on = 0;
    prev = 0;
    injected = k != BODY_HTML;
    pend = false;
    ok = true;
    memset(hot, 0, sizeof(hot));
    for (const char *p = k == BODY_HTML ? "hH/<>iIcC" : k == BODY_CSS ? "hH/" : "hH"; *p; p++) hot[(uint8_t)*p] = true;
  }
  bool feed(const char *d, size_t len) {
    while (len && ok) {
      size_t t = sizeof(w) - n < len ? sizeof(w) - n : len;
      memcpy(w + n, d, t);
      n += t;
      d += t;
      len -= t;
      process(false);
    }
    return ok;
  }
  bool finish() {
    process(true);
    flush();
    return ok;
  }

 private:
  static const size_t LOOK = 288;  // longest token that must be seen whole
  BodyKind kind;
  Sink sink;
  void *ctx;
  char w[1436], o[1424];  // o plus chunk framing fits one TCP segment
  size_t n, on;
  char prev;
  bool injected, pend, ok;
  bool hot[256];  // characters that can start a token; everything else is copied in runs

  void flush() {
    if (on && ok) ok = sink(ctx, o, on);
    on = 0;
  }
  void put(const char *s, size_t l) {
    if (l) prev = s[l - 1];
    while (l) {
      if (on == sizeof(o)) flush();
      size_t t = sizeof(o) - on < l ? sizeof(o) - on : l;
      memcpy(o + on, s, t);
      on += t;
      s += t;
      l -= t;
    }
  }
  void put(const std::string &s) { put(s.data(), s.size()); }
  void inject() {
    injected = true;
    pend = false;
    put(std::string("<script src=\"/__px/hook.js?7\"></script>"));
  }

  // Match an absolute URL's origin at s. Returns bytes consumed, 0 if nothing to rewrite.
  size_t url(const char *s, size_t len) {
    size_t i = 0;
    bool https = true, esc = false, rel = false;
    if (len > 8 && ieq(s, "https:", 6)) i = 6;
    else if (len > 7 && ieq(s, "http:", 5)) i = 5, https = false;
    else if (s[0] == '/') rel = true;
    else return 0;
    if (len > i + 2 && s[i] == '/' && s[i + 1] == '/') i += 2;
    else if (!rel && len > i + 4 && !memcmp(s + i, "\\/\\/", 4)) i += 4, esc = true;
    else return 0;
    size_t h = i;
    while (i < len && hostChar(s[i])) i++;
    size_t hl = i - h;
    if (!validHost(s + h, hl) || endsPx(s + h, hl)) return 0;
    // Namespace identifiers, not fetchable URLs.
    if (hl == 10 && (ieq(s + h, "www.w3.org", 10) || ieq(s + h, "schema.org", 10))) return 0;
    // YouTube's player rejects video URLs on any other host. The hook maps them when they are requested.
    if (hl > 15 && ieq(s + h + hl - 15, "googlevideo.com", 15)) return 0;
    if (i < len && s[i] == '@') return 0;
    if (rel && i < len && !strchr("/\"') >?#:", s[i])) return 0;
    int port = 0;
    if (i + 1 < len && s[i] == ':' && isdigit((unsigned char)s[i + 1])) {
      i++;
      while (i < len && isdigit((unsigned char)s[i]) && port < 65536) port = port * 10 + (s[i++] - '0');
      if (port > 65535) return 0;
    }
    put(std::string(esc ? "http:\\/\\/" : "http://") + mapHost(https, std::string(s + h, hl), port));
    return i;
  }

  void process(bool fin) {
    size_t i = 0;
    while (i < n && ok && (fin || n - i >= LOOK)) {
      size_t stop = fin ? n : n - LOOK + 1, run = i;
      while (run < stop && !hot[(uint8_t)w[run]]) run++;
      if (run > i) {
        put(w + i, run - i);
        i = run;
        continue;
      }
      const char *s = w + i;
      size_t l = n - i, used = 0;
      char c = *s;
      bool quoted = prev == '"' || prev == '\'';
      if ((c == 'h' || c == 'H') && !isalnum((unsigned char)prev) && prev != '/' && prev != '_' && prev != '.') {
        used = url(s, l);
      } else if (c == '/' && kind != BODY_JS && l > 3 && s[1] == '/' && (quoted || prev == '(' || (prev == '=' && kind == BODY_HTML))) {
        used = url(s, l);
      } else if (kind == BODY_HTML) {
        if (c == '<' && !injected && !pend && (s[1] == 'h' || s[1] == 'H' || s[1] == 's' || s[1] == 'S' || s[1] == 'b' || s[1] == 'B')) {
          if (l > 6 && ieq(s, "<head", 5) && (s[5] == '>' || isspace((unsigned char)s[5]))) pend = true;
          else if ((l > 7 && ieq(s, "<script", 7)) || (l > 5 && ieq(s, "<body", 5) && !isalnum((unsigned char)s[5]))) inject();
        } else if (c == '<' && l > 8 && (ieq(s, "<img ", 5) || ieq(s, "<iframe ", 8))) {
          // Only fetch what is scrolled into view. Unquoted, so it is harmless inside script strings.
          used = s[2] == 'm' || s[2] == 'M' ? 5 : 8;
          put(s, used);
          put(std::string("loading=lazy "));
        } else if (c == '>' && pend) {
          put(s, 1);
          inject();
          i++;
          continue;
        } else if ((c == 'i' || c == 'I') && (isspace((unsigned char)prev) || quoted) && l > 10 && ieq(s, "integrity=", 10)) {
          put(std::string("data-px-integrity="));
          used = 10;
        } else if ((c == 'c' || c == 'C') && (quoted || prev == '=') && l >= 23 && ieq(s, "content-security-policy", 23)) {
          put(std::string("x-removed-policy"));
          used = 23;
        }
      }
      if (!used) put(s, used = 1);
      i += used;
    }
    memmove(w, w + i, n - i);
    n -= i;
  }
};

static bool appendSink(void *c, const char *d, size_t l) {
  ((std::string *)c)->append(d, l);
  return true;
}

// Location / Refresh header values.
static std::string rewriteString(const std::string &s) {
  std::string out, in = s.compare(0, 2, "//") == 0 ? "https:" + s : s;
  auto *r = new Rewriter;
  r->begin(BODY_CSS, appendSink, &out);
  r->feed(in.data(), in.size());
  r->finish();
  delete r;
  return out;
}

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Cookies must survive plain http: no Secure, SameSite=None, or __Host-/__Secure- prefix.
static std::string rewriteSetCookie(const std::string &v, bool https, int port) {
  std::string out;
  bool first = true;
  for (size_t p = 0; p <= v.size();) {
    size_t e = v.find(';', p);
    if (e == std::string::npos) e = v.size();
    std::string a = trim(v.substr(p, e - p));
    p = e + 1;
    if (first) {
      if (a.compare(0, 7, "__Host-") == 0 || a.compare(0, 9, "__Secure-") == 0) a = "x" + a.substr(2);
      out = a;
      first = false;
      continue;
    }
    size_t eq = a.find('=');
    std::string k = trim(a.substr(0, eq));
    for (auto &c : k) c = tolower((unsigned char)c);
    if (k.empty() || k == "secure" || k == "samesite" || k == "partitioned") continue;
    if (k == "domain") {
      std::string d = eq == std::string::npos ? "" : trim(a.substr(eq + 1));
      if (!d.empty() && d[0] == '.') d.erase(0, 1);
      if (!validHost(d.c_str(), d.size())) continue;
      a = "Domain=" + mapHost(https, d, port);
    }
    out += "; " + a;
  }
  return out;
}

static std::string unmapCookies(std::string v) {
  for (size_t i = 0; i < v.size(); i++) {
    if (i && v[i - 1] != ' ' && v[i - 1] != ';') continue;
    if (v.compare(i, 6, "xHost-") == 0 || v.compare(i, 8, "xSecure-") == 0) v.replace(i, 1, "__");
  }
  return v;
}
