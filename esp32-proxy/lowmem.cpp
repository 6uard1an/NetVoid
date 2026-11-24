// Memory savings for an ESP32 without PSRAM. Linked in with -Wl,--wrap (see build.py).
//
// The prebuilt mbedTLS gives each connection a 16 KB buffer in both directions. Servers may send
// 16 KB records, so the input buffer must stay. But this proxy never writes more than 1400 bytes at
// once (upWrite) and sends no client certificate, so 4 KB is ample for the output buffer.
#include <Arduino.h>
#include <esp_wifi.h>
#include <lwip/priv/tcp_priv.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <new>

#define SMALL_OUT 4096
static void *shrunk[4];
static int nth = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Called before each handshake; handshakes are serialised, and ssl_setup allocates input, then output.
static volatile bool fresh = false;
void tlsBuffersBegin() {
  nth = 0;
  fresh = true;
}

// TLS session resumption: a reconnect to a recent host skips the certificate exchange and key agreement.
struct Saved {
  char host[128];
  mbedtls_ssl_session s;
  uint32_t used;
};
static Saved *saved[10];
static char curHost[128];
static volatile bool capture = false;
volatile uint32_t tlsResumeTried = 0;

extern "C" {
void *__real_mbedtls_calloc(size_t n, size_t size);
void *__wrap_mbedtls_calloc(size_t n, size_t size) {
  if (n != 1 || size < 16384 || size > 17500 || ++nth != 2) return __real_mbedtls_calloc(n, size);
  int slot = -1;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < 4 && slot < 0; i++)
    if (!shrunk[i]) shrunk[slot = i] = (void *)1;
  portEXIT_CRITICAL(&mux);
  if (slot < 0) return __real_mbedtls_calloc(n, size);
  return shrunk[slot] = __real_mbedtls_calloc(1, SMALL_OUT);
}

int __real_mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *h);
int __wrap_mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *h) {
  if (fresh && h) strlcpy(curHost, strlen(h) < sizeof(curHost) ? h : "", sizeof(curHost));
  return __real_mbedtls_ssl_set_hostname(ssl, h);
}

int __real_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
  if (fresh) {
    fresh = false;
    capture = true;
    for (auto p : saved)
      if (p && curHost[0] && !strcmp(p->host, curHost)) {
        p->used = millis();
        if (mbedtls_ssl_set_session(ssl, &p->s) == 0) tlsResumeTried = tlsResumeTried + 1;
      }
  }
  int r = __real_mbedtls_ssl_handshake(ssl);
  if (r == 0 && capture) {
    capture = false;
    // Verified already; the parsed certificate is only a few KB of dead weight from here on.
    mbedtls_ssl_session *s = ssl->private_session;
    if (s && s->private_peer_cert) {
      mbedtls_x509_crt_free(s->private_peer_cert);
      mbedtls_free(s->private_peer_cert);
      s->private_peer_cert = nullptr;
    }
    Saved **slot = &saved[0];
    for (auto &p : saved) {
      if (p && curHost[0] && !strcmp(p->host, curHost)) {
        slot = &p;
        break;
      }
      if (*slot && (!p || (int32_t)(p->used - (*slot)->used) < 0)) slot = &p;
    }
    if (!*slot) {
      *slot = new (std::nothrow) Saved;
      if (!*slot) return r;
      mbedtls_ssl_session_init(&(*slot)->s);
    }
    mbedtls_ssl_session_free(&(*slot)->s);
    mbedtls_ssl_session_init(&(*slot)->s);
    strcpy((*slot)->host, curHost);
    (*slot)->used = millis();
    if (mbedtls_ssl_get_session(ssl, &(*slot)->s) != 0) (*slot)->host[0] = 0;
  }
  return r;
}

// Connections from browsers: the stock stack waits 3 s before the first retransmission. (Enlarging its
// windows this way was tried and crashes: closing a connection assumes the compiled-in sizes.)
struct tcp_pcb *__real_tcp_alloc(u8_t prio);
struct tcp_pcb *__wrap_tcp_alloc(u8_t prio) {
  struct tcp_pcb *p = __real_tcp_alloc(prio);
  if (p) {
    p->rto = p->sv = 2;  // 500 ms ticks
  }
  return p;
}

// The default transmit queue can grow to about 50 KB.
esp_err_t __real_esp_wifi_init(const wifi_init_config_t *c);
esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *c) {
  wifi_init_config_t k = *c;
  // These buffers must stay generous: too few and the link drops packets under heavy download, so TLS
  // handshakes fail and pages stall. On this no-PSRAM chip that is the limit; do not lower them.
  k.dynamic_rx_buf_num = 32;
  k.static_rx_buf_num = 5;
  k.rx_ba_win = 5;
  k.dynamic_tx_buf_num = 10;
  return __real_esp_wifi_init(&k);
}

void __real_mbedtls_zeroize_and_free(void *p, size_t len);
void __wrap_mbedtls_zeroize_and_free(void *p, size_t len) {
  if (p)
    for (auto &s : shrunk)
      if (s == p) {
        s = nullptr;
        if (len > SMALL_OUT) len = SMALL_OUT;  // never grow: the slot may be stale after a failed setup
        break;
      }
  __real_mbedtls_zeroize_and_free(p, len);
}
}
