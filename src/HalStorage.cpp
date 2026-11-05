#ifdef _WIN32
// 64-bit off_t and lseek, as on the other hosts. Must precede every include.
#define _FILE_OFFSET_BITS 64
#endif

#include "HalStorage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "SimFs.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <sstream>
#include <vector>

HalStorage HalStorage::instance;
HalStorage::HalStorage() {}

namespace {
std::string configuredStorageRoot() {
  std::string root = sim_fs::getEnv("CROSSPOINT_SIM_SD");
  if (root.empty()) {
    root = sim_fs::getEnv("CROSSPOINT_EMU_SD");
  }
  if (root.empty()) {
    return "./fs_";
  }
#ifdef _WIN32
  // The rest of this file splits paths on '/' only.
  for (char &c : root) {
    if (c == '\\')
      c = '/';
  }
#endif
  return root;
}

bool containsUnsafeSegment(const std::string &path) {
#ifdef _WIN32
  // Win32 also treats a backslash as a separator, so it could escape the root.
  if (path.find('\\') != std::string::npos) {
    return true;
  }
#endif
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (segment == "..") {
      return true;
    }
  }
  return false;
}

std::string resolveStoragePath(const char *path) {
  std::string logical = path ? std::string(path) : std::string("/");
  if (logical.empty()) {
    logical = "/";
  }
  if (containsUnsafeSegment(logical)) {
    fprintf(stderr, "[SIM] rejected unsafe storage path: %s\n",
            logical.c_str());
    return {};
  }
  while (!logical.empty() && logical.front() == '/') {
    logical.erase(logical.begin());
  }

  std::string root = configuredStorageRoot();
  while (root.size() > 1 && root.back() == '/') {
    root.pop_back();
  }
  if (logical.empty()) {
    return root;
  }
  return root + "/" + logical;
}

bool ensureParentDirectories(const std::string &full) {
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  const std::string parent = full.substr(0, slash);
  if (parent.empty()) {
    return true;
  }
  for (size_t i = 1; i < parent.size(); ++i) {
    if (parent[i] == '/') {
      sim_fs::mkdir(parent.substr(0, i).c_str());
    }
  }
  return sim_fs::mkdir(parent.c_str()) == 0 || errno == EEXIST;
}

uint32_t packedFatTime(time_t timestamp) {
  struct tm local{};
#ifdef _WIN32
  const bool haveLocal = sim_fs::localTime(timestamp, &local);
#else
  const bool haveLocal = localtime_r(&timestamp, &local) != nullptr;
#endif
  if (!haveLocal || local.tm_year < 80 || local.tm_year > 207)
    return 0;
  const uint32_t date = static_cast<uint32_t>(
      ((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  const uint32_t time = static_cast<uint32_t>(
      (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
  return (date << 16) | time;
}
} // namespace

bool HalStorage::begin() {
  const std::string root = configuredStorageRoot();
  for (size_t i = 1; i < root.size(); ++i) {
    if (root[i] == '/') {
      sim_fs::mkdir(root.substr(0, i).c_str());
    }
  }
  return sim_fs::mkdir(root.c_str()) == 0 || errno == EEXIST;
}
bool HalStorage::ready() const { return true; }
void HalStorage::shutdown() {}

class HalFile::Impl {
public:
  int fd = -1;
  std::string path;
  sim_fs::DIR *dir = nullptr;
  size_t directoryPosition = 0;

  bool open(const char *p, int flags) {
    path = p;
    // The simulator's FsApiConstants.h just includes <fcntl.h> and typedef int
    // oflag_t, so all O_* constants are already native POSIX values — pass them
    // straight through.
#ifdef _WIN32
    fd = sim_fs::open(path.c_str(), flags);
#else
    fd = ::open(path.c_str(), flags, 0666);
#endif
    if (fd < 0) {
      fprintf(stderr, "[SIM] open failed: %s (flags=0x%x errno=%d %s)\n",
              path.c_str(), flags, errno, strerror(errno));
    }
    return fd >= 0;
  }

  bool openAsDir(const char *p) {
    path = p;
    dir = sim_fs::opendir(p);
    return dir != nullptr;
  }

  bool isDir() const { return dir != nullptr; }
  bool isOpen() const { return fd >= 0 || dir != nullptr; }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() {
  if (impl && impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
}
HalFile::HalFile(HalFile &&other) : impl(std::move(other.impl)) {}
HalFile &HalFile::operator=(HalFile &&other) {
  if (this != &other) {
    if (impl && impl->fd >= 0) {
      ::close(impl->fd);
      impl->fd = -1;
    }
    impl = std::move(other.impl);
  }
  return *this;
}

void HalFile::flush() {
  if (impl && impl->fd >= 0)
    sim_fs::fsync(impl->fd);
}
bool HalFile::sync() {
  if (!impl || impl->fd < 0)
    return false;
  return sim_fs::fsync(impl->fd) == 0;
}
size_t HalFile::getName(char *name, size_t len) {
  if (!impl || impl->path.empty())
    return 0;
  size_t slash = impl->path.rfind('/');
  std::string fname =
      (slash == std::string::npos) ? impl->path : impl->path.substr(slash + 1);
  size_t n = std::min(fname.size(), len - 1);
  memcpy(name, fname.c_str(), n);
  name[n] = '\0';
  return n;
}
size_t HalFile::size() {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return end < 0 ? 0 : (size_t)end;
}
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return size(); }
uint32_t HalFile::creationTime() {
  if (!impl || impl->fd < 0)
    return 0;
#if defined(__APPLE__)
  struct stat metadata{};
  if (fstat(impl->fd, &metadata) != 0)
    return 0;
  return packedFatTime(metadata.st_birthtimespec.tv_sec);
#else
  // POSIX does not expose file birth time portably. Let Library use its
  // first-seen fallback rather than misreporting a metadata change as creation.
  return 0;
#endif
}
uint32_t HalFile::modificationTime() {
  if (!impl || impl->fd < 0)
    return 0;
  sim_fs::Stat metadata{};
  if (sim_fs::fstat(impl->fd, &metadata) != 0)
    return 0;
  // Match HalFile's packed FAT date/time, including its two-second precision.
#ifdef _WIN32
  return packedFatTime(static_cast<time_t>(metadata.st_mtime_sec));
#else
  return packedFatTime(metadata.getMtime());
#endif
}

bool HalFile::seek(size_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)pos, SEEK_SET) >= 0;
}
bool HalFile::seek64(uint64_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  if (pos > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    return false;
  return lseek(impl->fd, static_cast<off_t>(pos), SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_CUR) >= 0;
}
bool HalFile::seekSet(size_t offset) {
  if (impl && impl->dir) {
    // libc telldir cookies need not survive closing the DIR stream. Keep a
    // consumed-entry count instead, so Library's close/reopen traversal works
    // on macOS as well as Linux. Hardware uses SdFat's native directory offset.
    rewindDirectory();
    while (impl->directoryPosition < offset) {
      if (!sim_fs::readdir(impl->dir))
        return false;
      ++impl->directoryPosition;
    }
    return true;
  }
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_SET) >= 0;
}
int HalFile::available() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return (int)(end - cur);
}
size_t HalFile::position() const {
  if (impl && impl->dir)
    return impl->directoryPosition;
  if (!impl || impl->fd < 0)
    return 0;
  off_t pos = lseek(impl->fd, 0, SEEK_CUR);
  return pos < 0 ? 0 : (size_t)pos;
}
int HalFile::read(void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return -1;
  ssize_t n = ::read(impl->fd, buf, count);
  return (int)n;
}
int HalFile::read() {
  if (!impl || impl->fd < 0)
    return -1;
  uint8_t c;
  return (::read(impl->fd, &c, 1) == 1) ? c : -1;
}
size_t HalFile::write(const void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return 0;
  ssize_t n = ::write(impl->fd, buf, count);
  return n < 0 ? 0 : (size_t)n;
}
size_t HalFile::write(const uint8_t *buf, size_t count) {
  return write(static_cast<const void *>(buf), count);
}
size_t HalFile::write(uint8_t b) {
  if (!impl || impl->fd < 0)
    return 0;
  return (::write(impl->fd, &b, 1) == 1) ? 1 : 0;
}
bool HalFile::rename(const char *newPath) {
  if (!impl || impl->path.empty()) {
    return false;
  }
  const std::string resolved = resolveStoragePath(newPath);
  if (resolved.empty()) {
    return false;
  }
  close();
  ensureParentDirectories(resolved);
  return sim_fs::rename(impl->path.c_str(), resolved.c_str()) == 0;
}
bool HalFile::isDirectory() const { return impl && impl->isDir(); }
void HalFile::rewindDirectory() {
  if (impl && impl->dir) {
    sim_fs::rewinddir(impl->dir);
    impl->directoryPosition = 0;
  }
}
bool HalFile::close() {
  if (!impl)
    return true;
  if (impl->dir) {
    sim_fs::closedir(impl->dir);
    impl->dir = nullptr;
  }
  if (impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
  return true;
}
HalFile HalFile::openNextFile() {
  if (!impl || !impl->dir)
    return HalFile();
  while (true) {
    sim_fs::dirent *entry = sim_fs::readdir(impl->dir);
    if (!entry)
      return HalFile();
    ++impl->directoryPosition;
    if (entry->d_name[0] == '.')
      continue; // skip . and ..

    std::string childFsPath = impl->path;
    if (childFsPath.back() != '/')
      childFsPath += '/';
    childFsPath += entry->d_name;

    HalFile child;
    sim_fs::Stat st;
    if (sim_fs::stat(childFsPath.c_str(), &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      child.impl->openAsDir(childFsPath.c_str());
    } else {
      child.impl->open(childFsPath.c_str(), O_RDONLY);
    }
    return child;
  }
}
bool HalFile::isOpen() const {
  if (!impl)
    return false;
  return impl->isOpen();
}
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char *path, const oflag_t oflag) {
  std::string full = resolveStoragePath(path);
  HalFile f;
  if (full.empty()) {
    return f;
  }
  if ((oflag & O_CREAT) != 0) {
    ensureParentDirectories(full);
  }
  sim_fs::Stat st;
  if (sim_fs::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    f.impl->openAsDir(full.c_str());
  } else {
    f.impl->open(full.c_str(), oflag);
  }
  return f;
}
bool HalStorage::mkdir(const char *path, const bool /*pFlag*/) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  // Create all intermediate directories (mkdir -p semantics).
  for (size_t i = 1; i < full.size(); ++i) {
    if (full[i] == '/') {
      // Ignore errors (may already exist).
      sim_fs::mkdir(full.substr(0, i).c_str());
    }
  }
  return sim_fs::mkdir(full.c_str()) == 0 || errno == EEXIST;
}
bool HalStorage::exists(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  sim_fs::Stat buffer;
  return (sim_fs::stat(full.c_str(), &buffer) == 0);
}
bool HalStorage::remove(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return sim_fs::remove(full.c_str()) == 0;
}
bool HalStorage::rename(const char *oldPath, const char *newPath) {
  std::string o = resolveStoragePath(oldPath);
  std::string n = resolveStoragePath(newPath);
  if (o.empty() || n.empty()) {
    return false;
  }
  ensureParentDirectories(n);
  return sim_fs::rename(o.c_str(), n.c_str()) == 0;
}
static bool removeDirRecursive(const std::string &full) {
  sim_fs::DIR *d = sim_fs::opendir(full.c_str());
  if (!d)
    return sim_fs::remove(full.c_str()) == 0; // might be a plain file
  sim_fs::dirent *entry;
  while ((entry = sim_fs::readdir(d)) != nullptr) {
    if (entry->d_name[0] == '.')
      continue;
    std::string child = full + "/" + entry->d_name;
    sim_fs::Stat st;
    if (sim_fs::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      removeDirRecursive(child);
    } else {
      sim_fs::remove(child.c_str());
    }
  }
  sim_fs::closedir(d);
  return sim_fs::rmdir(full.c_str()) == 0;
}

bool HalStorage::rmdir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}
bool HalStorage::removeDir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}

String HalStorage::readFile(const char *path) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return String("");
  size_t s = f.size();
  std::string content(s, '\0');
  f.read((void *)content.data(), s);
  return String(content);
}
bool HalStorage::readFileToStream(const char *path, Print &out,
                                  size_t chunkSize) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return false;
  std::vector<char> buf(chunkSize);
  int n;
  while ((n = f.read(buf.data(), chunkSize)) > 0) {
    out.write(reinterpret_cast<const uint8_t *>(buf.data()), n);
  }
  return true;
}
size_t HalStorage::readFileToBuffer(const char *path, char *buffer,
                                    size_t bufferSize, size_t maxBytes) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return 0;
  size_t toRead = bufferSize - 1;
  if (maxBytes > 0 && maxBytes < toRead)
    toRead = maxBytes;
  int n = f.read(buffer, toRead);
  if (n < 0)
    n = 0;
  buffer[n] = '\0';
  return n;
}
bool HalStorage::writeFile(const char *path, const String &content) {
  HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f)
    return false;
  f.write(content.c_str(), content.length());
  return true;
}
bool HalStorage::ensureDirectoryExists(const char *path) { return mkdir(path); }

void HalStorage::installDateTimeCallback(
    const uint8_t *utcOffsetQuarterHoursBiased) {
  (void)utcOffsetQuarterHoursBiased;
}

bool HalStorage::openFileForRead(const char *moduleName, const char *path,
                                 HalFile &file) {
  file = open(path, O_RDONLY);
  return file.isOpen();
}
bool HalStorage::openFileForRead(const char *moduleName,
                                 const std::string &path, HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char *moduleName, const String &path,
                                 HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const char *path,
                                  HalFile &file) {
  file = open(path, O_RDWR | O_CREAT | O_TRUNC);
  return file.isOpen();
}
bool HalStorage::openFileForWrite(const char *moduleName,
                                  const std::string &path, HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const String &path,
                                  HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char *path, int maxFiles) {
  std::vector<String> result;
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return result;
  }
  sim_fs::DIR *dir = sim_fs::opendir(full.c_str());
  if (!dir)
    return result;
  sim_fs::dirent *entry;
  while ((entry = sim_fs::readdir(dir)) != nullptr &&
         (int)result.size() < maxFiles) {
    if (entry->d_name[0] == '.')
      continue; // skip . and ..
    result.push_back(String(entry->d_name));
  }
  sim_fs::closedir(dir);
  return result;
}
