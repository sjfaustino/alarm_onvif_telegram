#include "http_digest.h"
#include <cstring>

namespace {

// RFC 1321 MD5, small and allocation-free.
struct Md5 {
  uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
  uint8_t buf[64];
  uint64_t len = 0;

  static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
      m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) |
             ((uint32_t)p[i * 4 + 3] << 24);
    }
    uint32_t A = a, B = b, C = c, D = d;
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) { f = (B & C) | (~B & D); g = i; }
      else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { f = C ^ (B | ~D); g = (7 * i) % 16; }
      uint32_t tmp = D;
      D = C;
      C = B;
      B = B + rotl(A + f + K[i] + m[g], S[i]);
      A = tmp;
    }
    a += A; b += B; c += C; d += D;
  }

  void update(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      buf[len % 64] = p[i];
      len++;
      if (len % 64 == 0) block(buf);
    }
  }

  void final(uint8_t out[16]) {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (len % 64 != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = (uint8_t)(bits >> (8 * i));
    update(lenBytes, 8);
    uint32_t words[4] = {a, b, c, d};
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
  }
};

// Value of key=value or key="value" in a challenge, "" if absent.
String param(const String& challenge, const char* key) {
  String lower = challenge;
  lower.toLowerCase();
  String k = String(key) + "=";
  for (int p = lower.indexOf(k); p >= 0; p = lower.indexOf(k, p + 1)) {
    // Must start a parameter, so "nonce" doesn't match inside "cnonce".
    if (p > 0 && lower[p - 1] != ' ' && lower[p - 1] != ',') continue;
    int v = p + (int)k.length();
    if (v < (int)challenge.length() && challenge[v] == '"') {
      int end = challenge.indexOf('"', v + 1);
      return end > v ? challenge.substring(v + 1, end) : "";
    }
    int end = challenge.indexOf(',', v);
    String out = end >= 0 ? challenge.substring(v, end) : challenge.substring(v);
    out.trim();
    return out;
  }
  return "";
}

// Whether a qop list such as "auth,auth-int" includes plain "auth" ("auth-int"
// alone doesn't).
bool offersQopAuth(const String& qopOptions) {
  int start = 0;
  while (start <= (int)qopOptions.length()) {
    int comma = qopOptions.indexOf(',', start);
    String option = qopOptions.substring(start, comma < 0 ? qopOptions.length() : comma);
    option.trim();
    if (option == "auth") return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

}  // namespace

String md5Hex(const String& data) {
  Md5 md;
  md.update((const uint8_t*)data.c_str(), data.length());
  uint8_t digest[16];
  md.final(digest);
  static const char* hex = "0123456789abcdef";
  String out;
  for (uint8_t byte : digest) {
    out += hex[byte >> 4];
    out += hex[byte & 15];
  }
  return out;
}

String digestAuthorization(const String& challenge, const String& method, const String& uri, const String& user,
                           const String& pass, const String& cnonce) {
  String head = challenge.substring(0, 6);
  head.toLowerCase();
  if (head != "digest") return "";
  String algorithm = param(challenge, "algorithm");
  if (algorithm.length() > 0 && !algorithm.equalsIgnoreCase("MD5")) return "";
  String realm = param(challenge, "realm");
  String nonce = param(challenge, "nonce");
  String opaque = param(challenge, "opaque");
  String qopOptions = param(challenge, "qop");
  qopOptions.toLowerCase();
  bool useQop = qopOptions.length() > 0;
  if (useQop && !offersQopAuth(qopOptions)) return ""; // e.g. only auth-int offered

  String ha1 = md5Hex(user + ":" + realm + ":" + pass);
  String ha2 = md5Hex(method + ":" + uri);
  const char* nc = "00000001";
  String response = useQop ? md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2)
                           : md5Hex(ha1 + ":" + nonce + ":" + ha2);

  String out = "Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + uri +
               "\", response=\"" + response + "\"";
  if (algorithm.length() > 0) out += ", algorithm=" + algorithm;
  if (useQop) out += ", qop=auth, nc=" + String(nc) + ", cnonce=\"" + cnonce + "\"";
  if (opaque.length() > 0) out += ", opaque=\"" + opaque + "\"";
  return out;
}

String requestUriFromUrl(const String& url) {
  int scheme = url.indexOf("://");
  int start = scheme >= 0 ? scheme + 3 : 0;
  int slash = url.indexOf('/', start);
  return slash >= 0 ? url.substring(slash) : String("/");
}
