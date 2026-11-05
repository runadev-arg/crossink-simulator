#ifdef _WIN32

#include "SimSocketPlatform.h"

#include <climits>
#include <cstdio>
#include <mutex>

namespace sim_socket {

void init() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
      std::fprintf(stderr, "[SIM] WSAStartup failed: %d\n", result);
  });
}

int close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }

int shutdownBoth(int fd) {
  return ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
}

ssize_t send(int fd, const void *data, size_t len) {
  const int chunk = len > INT_MAX ? INT_MAX : static_cast<int>(len);
  return ::send(static_cast<SOCKET>(fd), static_cast<const char *>(data), chunk,
                0);
}

ssize_t recv(int fd, void *data, size_t len) {
  const int chunk = len > INT_MAX ? INT_MAX : static_cast<int>(len);
  return ::recv(static_cast<SOCKET>(fd), static_cast<char *>(data), chunk, 0);
}

int dup(int fd) {
  WSAPROTOCOL_INFOW info;
  if (WSADuplicateSocketW(static_cast<SOCKET>(fd), GetCurrentProcessId(),
                          &info) != 0)
    return -1;
  const SOCKET copy =
      WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                 &info, 0, WSA_FLAG_OVERLAPPED);
  return copy == INVALID_SOCKET ? -1 : static_cast<int>(copy);
}

void setTimeoutSeconds(int fd, int seconds) {
  // Unlike BSD's timeval, Winsock takes a DWORD of milliseconds here.
  const DWORD milliseconds = static_cast<DWORD>(seconds) * 1000;
  const char *value = reinterpret_cast<const char *>(&milliseconds);
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO, value,
             sizeof(milliseconds));
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO, value,
             sizeof(milliseconds));
}

void setListenerAddressOptions(int fd) {
  const BOOL yes = TRUE;
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char *>(&yes), sizeof(yes));
}

std::string lastError() {
  const int code = WSAGetLastError();
  char *message = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(code), 0,
                 reinterpret_cast<char *>(&message), 0, nullptr);
  std::string text = "WSA error " + std::to_string(code);
  if (message) {
    text += ": ";
    text += message;
    LocalFree(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();
  }
  return text;
}

} // namespace sim_socket

#endif // _WIN32
