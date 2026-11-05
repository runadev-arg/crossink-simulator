#pragma once

// Force-included into every translation unit of a Windows simulator build
// (-include), so firmware sources can stay untouched. Holds only what the UCRT
// lacks and firmware calls directly. Must stay valid C and C++, and must not pull
// in <windows.h>.

#ifdef _WIN32

// A file that wants rand_s defines this before <stdlib.h> (expat does), and this
// header has already included it by then.
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// glibc and BSD <sys/types.h> define it, and PNGdec's inflate.c relies on that.
typedef unsigned int uint;

#ifdef __cplusplus
extern "C" {
#endif

// The UCRT has no aligned_alloc: its aligned heap needs _aligned_free, and callers
// release this memory with free(). malloc is already 16-byte aligned on x64, which
// covers alignof(std::max_align_t), the only alignment the firmware asks for. A
// stricter request fails visibly as an allocation failure.
static inline void *aligned_alloc(size_t alignment, size_t size) {
  return alignment <= 16 ? malloc(size) : NULL;
}

// BSD string functions that Arduino-ESP32's newlib, macOS and libbsd provide.
static inline size_t strlcpy(char *dst, const char *src, size_t size) {
  const size_t length = strlen(src);
  if (size > 0) {
    const size_t copied = length < size - 1 ? length : size - 1;
    memcpy(dst, src, copied);
    dst[copied] = '\0';
  }
  return length;
}

static inline size_t strlcat(char *dst, const char *src, size_t size) {
  const size_t existing = strnlen(dst, size);
  if (existing == size)
    return size + strlen(src);
  return existing + strlcpy(dst + existing, src, size - existing);
}

#ifdef __cplusplus
}

// std::filesystem::path::c_str() is wchar_t on Windows, and simulator-only firmware
// code hands it to std::fopen. Declared before <cstdio> so that its
// `using ::fopen` picks this overload up.
#include <stdio.h>
#include <wchar.h>
inline FILE *fopen(const wchar_t *path, const char *mode) {
  wchar_t wideMode[8] = {0};
  for (int i = 0; i < 7 && mode[i]; ++i)
    wideMode[i] = static_cast<wchar_t>(mode[i]);
  return _wfopen(path, wideMode);
}
#endif

#endif // _WIN32
