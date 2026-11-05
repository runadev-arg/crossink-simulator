#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "SimFs.h"

namespace sim_fs {
namespace {

std::wstring widen(const std::string &utf8) {
  if (utf8.empty())
    return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), needed);
  return out;
}

std::string narrow(const wchar_t *wide) {
  if (!wide || !*wide)
    return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr,
                      nullptr);
  out.resize(static_cast<size_t>(needed - 1));
  return out;
}

// Long paths need the \\?\ form, which requires an absolute backslashed path.
std::wstring winPath(const char *path) {
  std::wstring wide = widen(path ? path : "");
  for (wchar_t &c : wide) {
    if (c == L'/')
      c = L'\\';
  }
  if (wide.size() < 240)
    return wide;
  std::wstring full(32768, L'\0');
  const DWORD length = GetFullPathNameW(wide.c_str(), 32768, full.data(), nullptr);
  if (length == 0 || length >= 32768)
    return wide;
  full.resize(length);
  if (full.rfind(L"\\\\", 0) == 0)
    return full;
  return L"\\\\?\\" + full;
}

int setErrnoFromLastError() {
  switch (GetLastError()) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
    errno = ENOENT;
    break;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    errno = EEXIST;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
    errno = EACCES;
    break;
  case ERROR_DIR_NOT_EMPTY:
    errno = ENOTEMPTY;
    break;
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    errno = ENOSPC;
    break;
  default:
    errno = EIO;
    break;
  }
  return -1;
}

// Antivirus and the search indexer briefly hold freshly written files. POSIX
// hosts never see that, so retry before reporting a failure to the firmware.
template <typename Fn> bool retryTransient(Fn &&fn) {
  for (int attempt = 0;; ++attempt) {
    if (fn())
      return true;
    const DWORD error = GetLastError();
    if (attempt >= 20 ||
        (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION))
      return false;
    Sleep(25);
  }
}

int64_t unixSeconds(const FILETIME &time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return static_cast<int64_t>(value.QuadPart / 10000000ULL) - 11644473600LL;
}

} // namespace

struct DIR {
  std::wstring pattern;
  std::vector<std::string> names;
  size_t next = 0;
  dirent current{};
};

namespace {
bool enumerate(DIR &dir) {
  dir.names.clear();
  dir.next = 0;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(dir.pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    setErrnoFromLastError();
    return false;
  }
  do {
    dir.names.push_back(narrow(data.cFileName));
  } while (FindNextFileW(find, &data));
  // The listing is a snapshot and no handle stays open: on Windows an open
  // find handle would stop the firmware renaming or removing the directory.
  FindClose(find);
  return true;
}
} // namespace

DIR *opendir(const char *path) {
  const std::wstring base = winPath(path);
  const DWORD attributes = GetFileAttributesW(base.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    setErrnoFromLastError();
    return nullptr;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    errno = ENOTDIR;
    return nullptr;
  }
  auto *dir = new DIR();
  dir->pattern = base;
  if (!dir->pattern.empty() && dir->pattern.back() != L'\\')
    dir->pattern += L'\\';
  dir->pattern += L'*';
  if (!enumerate(*dir)) {
    delete dir;
    return nullptr;
  }
  return dir;
}

dirent *readdir(DIR *dir) {
  if (!dir || dir->next >= dir->names.size())
    return nullptr;
  const std::string &name = dir->names[dir->next++];
  const size_t length =
      name.size() < sizeof(dir->current.d_name) - 1
          ? name.size()
          : sizeof(dir->current.d_name) - 1;
  std::memcpy(dir->current.d_name, name.data(), length);
  dir->current.d_name[length] = '\0';
  return &dir->current;
}

void rewinddir(DIR *dir) {
  if (dir)
    enumerate(*dir);
}

int closedir(DIR *dir) {
  delete dir;
  return 0;
}

int stat(const char *path, Stat *out) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(winPath(path).c_str(), GetFileExInfoStandard,
                            &data))
    return setErrnoFromLastError();
  ULARGE_INTEGER size;
  size.LowPart = data.nFileSizeLow;
  size.HighPart = data.nFileSizeHigh;
  out->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR
                                                                    : _S_IFREG;
  out->st_size = static_cast<int64_t>(size.QuadPart);
  out->st_mtime_sec = unixSeconds(data.ftLastWriteTime);
  return 0;
}

int fstat(int fd, Stat *out) {
  BY_HANDLE_FILE_INFORMATION info;
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandle(handle, &info))
    return setErrnoFromLastError();
  ULARGE_INTEGER size;
  size.LowPart = info.nFileSizeLow;
  size.HighPart = info.nFileSizeHigh;
  out->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR
                                                                    : _S_IFREG;
  out->st_size = static_cast<int64_t>(size.QuadPart);
  out->st_mtime_sec = unixSeconds(info.ftLastWriteTime);
  return 0;
}

int open(const char *path, int flags) {
  DWORD access = GENERIC_READ;
  switch (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
  case _O_WRONLY:
    access = GENERIC_WRITE;
    break;
  case _O_RDWR:
    access = GENERIC_READ | GENERIC_WRITE;
    break;
  default:
    break;
  }

  DWORD disposition = OPEN_EXISTING;
  const bool create = (flags & _O_CREAT) != 0;
  const bool truncate = (flags & _O_TRUNC) != 0;
  if (create && (flags & _O_EXCL))
    disposition = CREATE_NEW;
  else if (create && truncate)
    disposition = CREATE_ALWAYS;
  else if (create)
    disposition = OPEN_ALWAYS;
  else if (truncate)
    disposition = TRUNCATE_EXISTING;

  const std::wstring wide = winPath(path);
  HANDLE handle = INVALID_HANDLE_VALUE;
  // Full sharing mirrors POSIX, where an open file does not block other opens,
  // renames or removes. The handle is not inheritable.
  const bool opened = retryTransient([&] {
    handle = CreateFileW(wide.c_str(), access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle != INVALID_HANDLE_VALUE;
  });
  if (!opened)
    return setErrnoFromLastError();

  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                 flags & _O_APPEND);
  if (fd < 0) {
    CloseHandle(handle);
    errno = EMFILE;
    return -1;
  }
  // Text-mode translation would corrupt EPUBs, caches and downloads.
  _setmode(fd, _O_BINARY);
  return fd;
}

int mkdir(const char *path) {
  return CreateDirectoryW(winPath(path).c_str(), nullptr)
             ? 0
             : setErrnoFromLastError();
}

int rmdir(const char *path) {
  const std::wstring wide = winPath(path);
  return retryTransient([&] { return RemoveDirectoryW(wide.c_str()) != 0; })
             ? 0
             : setErrnoFromLastError();
}

int remove(const char *path) {
  const std::wstring wide = winPath(path);
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return setErrnoFromLastError();
  if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    return rmdir(path);
  return retryTransient([&] { return DeleteFileW(wide.c_str()) != 0; })
             ? 0
             : setErrnoFromLastError();
}

int rename(const char *from, const char *to) {
  const std::wstring source = winPath(from);
  const std::wstring target = winPath(to);
  return retryTransient([&] {
           return MoveFileExW(source.c_str(), target.c_str(),
                              MOVEFILE_REPLACE_EXISTING |
                                  MOVEFILE_WRITE_THROUGH) != 0;
         })
             ? 0
             : setErrnoFromLastError();
}

int fsync(int fd) {
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  // A read-only handle has nothing to flush; POSIX fsync succeeds on it.
  if (!FlushFileBuffers(handle) && GetLastError() != ERROR_ACCESS_DENIED)
    return setErrnoFromLastError();
  return 0;
}

FILE *fopen(const char *path, const char *mode) {
  return _wfopen(winPath(path).c_str(), widen(mode).c_str());
}

bool localTime(int64_t timestamp, struct tm *out) {
  const __time64_t value = timestamp;
  return _localtime64_s(out, &value) == 0;
}

std::string getEnv(const char *name) {
  return narrow(_wgetenv(widen(name).c_str()));
}

bool diskFreeBytes(const char *path, uint64_t *free, uint64_t *total) {
  ULARGE_INTEGER available;
  ULARGE_INTEGER capacity;
  if (!GetDiskFreeSpaceExW(winPath(path).c_str(), &available, &capacity,
                           nullptr))
    return false;
  if (free)
    *free = available.QuadPart;
  if (total)
    *total = capacity.QuadPart;
  return true;
}

} // namespace sim_fs

#endif // _WIN32
