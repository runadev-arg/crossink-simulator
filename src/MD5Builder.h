#pragma once

#if defined(__APPLE__)
#include "MD5Builder_mac.h"
#elif defined(__linux__)
#include "MD5Builder_linux.h"
#elif defined(_WIN32)
#include "MD5Builder_win.h"
#else
#error "Unsupported host OS for simulator MD5Builder"
#endif
