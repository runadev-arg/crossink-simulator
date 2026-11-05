#pragma once

// Self-contained RFC 1321 MD5, so the Windows simulator needs neither OpenSSL
// nor <windows.h> in a header the firmware includes.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WString.h"

class MD5Builder {
public:
  MD5Builder() {
    begin();
    memset(digest_, 0, sizeof(digest_));
  }

  void begin() {
    state_[0] = 0x67452301u;
    state_[1] = 0xefcdab89u;
    state_[2] = 0x98badcfeu;
    state_[3] = 0x10325476u;
    bytes_ = 0;
    buffered_ = 0;
  }

  void add(const uint8_t *data, size_t len) {
    bytes_ += len;
    while (len > 0) {
      if (buffered_ == 0 && len >= sizeof(buffer_)) {
        transform(data);
        data += sizeof(buffer_);
        len -= sizeof(buffer_);
        continue;
      }
      const size_t take = len < sizeof(buffer_) - buffered_
                              ? len
                              : sizeof(buffer_) - buffered_;
      memcpy(buffer_ + buffered_, data, take);
      buffered_ += take;
      data += take;
      len -= take;
      if (buffered_ == sizeof(buffer_)) {
        transform(buffer_);
        buffered_ = 0;
      }
    }
  }

  void add(const char *str) {
    if (str)
      add(reinterpret_cast<const uint8_t *>(str), strlen(str));
  }

  void calculate() {
    const uint64_t bits = bytes_ * 8;
    static constexpr uint8_t kPadding[64] = {0x80};
    const size_t padding =
        buffered_ < 56 ? 56 - buffered_ : 64 + 56 - buffered_;
    add(kPadding, padding);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i)
      length[i] = static_cast<uint8_t>(bits >> (8 * i));
    add(length, sizeof(length));
    for (int i = 0; i < 4; ++i) {
      digest_[i * 4 + 0] = static_cast<uint8_t>(state_[i]);
      digest_[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 8);
      digest_[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 16);
      digest_[i * 4 + 3] = static_cast<uint8_t>(state_[i] >> 24);
    }
  }

  String toString() const {
    char hex[33];
    for (int i = 0; i < 16; i++) {
      snprintf(hex + i * 2, 3, "%02x", digest_[i]);
    }
    return String(hex);
  }

private:
  static uint32_t rotl(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void transform(const uint8_t *block) {
    static constexpr uint32_t kSine[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
        0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
        0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
        0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
        0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
        0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
        0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
        0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static constexpr int kShift[16] = {7, 12, 17, 22, 5, 9,  14, 20,
                                       4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t words[16];
    for (int i = 0; i < 16; ++i) {
      words[i] = static_cast<uint32_t>(block[i * 4]) |
                 static_cast<uint32_t>(block[i * 4 + 1]) << 8 |
                 static_cast<uint32_t>(block[i * 4 + 2]) << 16 |
                 static_cast<uint32_t>(block[i * 4 + 3]) << 24;
    }
    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      switch (i / 16) {
      case 0:
        f = (b & c) | (~b & d);
        g = i;
        break;
      case 1:
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
        break;
      case 2:
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
        break;
      default:
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
        break;
      }
      const uint32_t next = d;
      d = c;
      c = b;
      b = b + rotl(a + f + kSine[i] + words[g], kShift[(i / 16) * 4 + i % 4]);
      a = next;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  uint32_t state_[4];
  uint64_t bytes_ = 0;
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint8_t digest_[16];
};
