// Streaming gunzip whose 32 KB window lives in instruction RAM, which is otherwise unused on this chip
// but only allows aligned 32-bit access. That is why the decompressor in ROM cannot be used.
// Canonical Huffman decoding with a 9-bit lookup table, falling back to bit by bit for longer codes.
#pragma once
#include <stdint.h>
#include <string.h>

struct Gunzip {
  typedef int (*Src)(void *);  // next compressed byte, or -1
  typedef bool (*Out)(void *, const char *, size_t);

  volatile uint32_t *win;  // 8192 words
  Src src;
  Out out;
  void *srcCtx, *outCtx;

  // Returns true only if the whole stream was decoded and delivered.
  bool run() {
    pos = total = bitBuf = bitCnt = on = 0;
    bad = false;
    if (!header()) return false;
    int last;
    do {
      last = bits(1);
      int type = bits(2);
      if (bad) return false;
      if (type == 0) stored();
      else if (type == 1) fixedBlock();
      else if (type == 2) dynamicBlock();
      else bad = true;
      if (bad) return false;
    } while (!last);
    flush();
    align();
    for (int i = 0; i < 8; i++) bits(8);  // CRC and size
    return !bad;
  }

 private:
  struct Huff {
    uint16_t count[16], *symbol, fast[512];  // fast: length << 12 | symbol, 0 = longer code
  };
  uint32_t pos, total, bitBuf;
  int bitCnt;
  size_t on;
  bool bad;
  char ob[512];
  uint16_t lenSym[288], distSym[32];
  uint8_t lens[320];
  Huff L, D;

  int byte() {
    int c = bad ? -1 : src(srcCtx);
    if (c < 0) {
      bad = true;
      return 0;
    }
    return c;
  }
  int bits(int n) {
    while (bitCnt < n) {
      bitBuf |= (uint32_t)byte() << bitCnt;
      bitCnt += 8;
      if (bad) return 0;
    }
    int v = bitBuf & ((1u << n) - 1);
    bitBuf >>= n;
    bitCnt -= n;
    return v;
  }
  void flush() {
    if (on && !out(outCtx, ob, on)) bad = true;
    on = 0;
  }
  void emit(uint8_t c) {
    uint32_t sh = (pos & 3) * 8, w = win[pos >> 2];
    win[pos >> 2] = (w & ~(0xFFu << sh)) | (uint32_t)c << sh;
    pos = (pos + 1) & 32767;
    if (total < 32768) total++;
    ob[on++] = c;
    if (on == sizeof(ob)) flush();
  }
  uint8_t back(uint32_t dist) {
    uint32_t p = (pos - dist) & 32767;
    return win[p >> 2] >> ((p & 3) * 8);
  }
  bool header() {
    if (byte() != 0x1f || byte() != 0x8b || byte() != 8) return false;
    int f = byte();
    for (int i = 0; i < 6; i++) byte();
    if (f & 4) {
      int n = byte();
      n |= byte() << 8;
      while (n-- && !bad) byte();
    }
    for (int m = 8; m <= 16; m <<= 1)
      if (f & m)
        while (byte()) {}
    if (f & 2) byte(), byte();
    return !bad;
  }
  // Returns <0 if the lengths are over-subscribed.
  static int build(Huff &h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    int left = 1;
    for (int i = 1; i < 16; i++) {
      left = (left << 1) - h.count[i];
      if (left < 0) return left;
    }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++)
      if (len[i]) h.symbol[offs[len[i]]++] = i;
    memset(h.fast, 0, sizeof(h.fast));
    uint16_t next[16];
    int code = 0;
    h.count[0] = 0;
    for (int i = 1; i < 16; i++) next[i] = code = (code + h.count[i - 1]) << 1;
    for (int i = 0; i < n; i++) {
      int l = len[i];
      if (!l) continue;
      int c = next[l]++, rev = 0;
      if (l > 9) continue;
      for (int b = 0; b < l; b++) rev |= ((c >> b) & 1) << (l - 1 - b);
      for (; rev < 512; rev += 1 << l) h.fast[rev] = l << 12 | i;
    }
    h.count[0] = 0;
    return left;
  }
  int decode(const Huff &h) {
    // Looking 9 bits ahead never runs off the end: the 8 byte trailer follows the last code.
    while (bitCnt < 9) {
      bitBuf |= (uint32_t)byte() << bitCnt;
      bitCnt += 8;
      if (bad) return -1;
    }
    uint16_t f = h.fast[bitBuf & 511];
    if (f) {
      bitBuf >>= f >> 12;
      bitCnt -= f >> 12;
      return f & 4095;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
      if (!bitCnt) {
        bitBuf = byte();
        bitCnt = 8;
        if (bad) return -1;
      }
      code |= bitBuf & 1;
      bitBuf >>= 1;
      bitCnt--;
      int c = h.count[l];
      if (code - c < first) return h.symbol[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    bad = true;
    return -1;
  }
  void align() {
    bitBuf >>= bitCnt & 7;
    bitCnt &= ~7;
  }
  void stored() {
    align();
    int n = bits(16), m = bits(16);
    if ((n ^ m) != 0xFFFF) bad = true;
    while (n-- > 0 && !bad) emit(bits(8));
  }
  void codes() {
    static const uint16_t lb[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lx[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t db[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dx[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (!bad) {
      int s = decode(L);
      if (bad || s == 256) return;
      if (s < 256) {
        emit(s);
        continue;
      }
      s -= 257;
      if (s >= 29) break;
      int len = lb[s] + bits(lx[s]);
      s = decode(D);
      if (bad || s >= 30) break;
      uint32_t dist = db[s] + bits(dx[s]);
      if (dist > total) break;
      while (len-- && !bad) emit(back(dist));
    }
    bad = true;
  }
  void fixedBlock() {
    memset(lens, 8, 144);
    memset(lens + 144, 9, 112);
    memset(lens + 256, 7, 24);
    memset(lens + 280, 8, 8);
    L.symbol = lenSym;
    D.symbol = distSym;
    build(L, lens, 288);
    memset(lens, 5, 30);
    build(D, lens, 30);
    codes();
  }
  void dynamicBlock() {
    static const uint8_t order[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    L.symbol = lenSym;
    D.symbol = distSym;
    int nl = bits(5) + 257, nd = bits(5) + 1, nc = bits(4) + 4, i;
    if (nl > 286 || nd > 30) bad = true;
    if (bad) return;
    memset(lens, 0, sizeof(lens));
    for (i = 0; i < nc; i++) lens[order[i]] = bits(3);
    if (build(L, lens, 19) != 0) bad = true;
    memset(lens, 0, sizeof(lens));
    for (i = 0; i < nl + nd && !bad;) {
      int s = decode(L);
      if (bad) return;
      if (s < 16) {
        lens[i++] = s;
        continue;
      }
      uint8_t v = 0;
      int rep;
      if (s == 16) {
        if (!i) {
          bad = true;
          return;
        }
        v = lens[i - 1];
        rep = 3 + bits(2);
      } else rep = s == 17 ? 3 + bits(3) : 11 + bits(7);
      if (i + rep > nl + nd) {
        bad = true;
        return;
      }
      while (rep--) lens[i++] = v;
    }
    if (bad) return;
    int e = build(L, lens, nl);
    if (e < 0 || !lens[256]) bad = true;
    if (build(D, lens + nl, nd) < 0) bad = true;
    if (!bad) codes();
  }
};
