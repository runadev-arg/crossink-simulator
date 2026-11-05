#pragma once

// Host filesystem primitives used by the storage and HTTP shims.
//
// POSIX hosts pass straight through to libc. Windows implements them on the
// wide Win32 API so that paths are UTF-8 (the narrow CRT calls use the ANSI code
// page), files are always binary, open files can be renamed and removed the way
// POSIX allows, and rename() replaces an existing target (the firmware's
// write-to-temp-then-rename relies on that).
//
// This header must stay free of <windows.h>: it is included next to firmware
// code whose identifiers collide with Win32 macros.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sim_fs {

#ifdef _WIN32

struct dirent {
  char d_name[1024];
};
struct DIR;

DIR *opendir(const char *path);
dirent *readdir(DIR *dir);
void rewinddir(DIR *dir);
int closedir(DIR *dir);

struct Stat {
  uint32_t st_mode = 0;
  int64_t st_size = 0;
  int64_t st_mtime_sec = 0;
};
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

int stat(const char *path, Stat *out);
int fstat(int fd, Stat *out);
// `flags` are the CRT's O_* values; the descriptor is always binary and is not
// inherited by child processes.
int open(const char *path, int flags);
int mkdir(const char *path);
int rmdir(const char *path);
int remove(const char *path);
int rename(const char *from, const char *to);
int fsync(int fd);
FILE *fopen(const char *path, const char *mode);
bool localTime(int64_t timestamp, struct tm *out);
// getenv that returns UTF-8. Empty when unset.
std::string getEnv(const char *name);

// Free bytes available to this process on the volume holding `path`.
bool diskFreeBytes(const char *path, uint64_t *free, uint64_t *total);

#else

using ::DIR;
using ::dirent;

inline DIR *opendir(const char *path) { return ::opendir(path); }
inline dirent *readdir(DIR *dir) { return ::readdir(dir); }
inline void rewinddir(DIR *dir) { ::rewinddir(dir); }
inline int closedir(DIR *dir) { return ::closedir(dir); }

struct Stat : ::stat {
  int64_t getMtime() const { return st_mtime; }
};
inline int stat(const char *path, Stat *out) { return ::stat(path, out); }
inline int fstat(int fd, Stat *out) { return ::fstat(fd, out); }
inline int mkdir(const char *path) { return ::mkdir(path, 0777); }
inline int rmdir(const char *path) { return ::rmdir(path); }
inline int remove(const char *path) { return std::remove(path); }
inline int rename(const char *from, const char *to) {
  return std::rename(from, to);
}
inline int fsync(int fd) { return ::fsync(fd); }
inline FILE *fopen(const char *path, const char *mode) {
  return std::fopen(path, mode);
}
inline std::string getEnv(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

#endif

} // namespace sim_fs
