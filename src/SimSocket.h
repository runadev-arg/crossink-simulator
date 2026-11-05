#pragma once

// The socket calls that differ between BSD sockets and Winsock. Descriptors stay
// plain ints at this layer, as in the rest of the simulator.
//
// Translation units that need sockaddr & co. include SimSocketPlatform.h, which
// is the only place that pulls in the system socket headers.

#include <cstddef>
#include <string>

#ifdef _WIN32
#include <sys/types.h> // ssize_t
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace sim_socket {

#ifdef _WIN32

// Must run before the first socket or name-resolution call. Idempotent.
void init();
int close(int fd);
int shutdownBoth(int fd);
ssize_t send(int fd, const void *data, size_t len);
ssize_t recv(int fd, void *data, size_t len);
int dup(int fd);
void setTimeoutSeconds(int fd, int seconds);
// Lets a restarted server rebind. Winsock's SO_REUSEADDR would instead let two
// simulators share one port silently, so Windows asks for exclusive use.
void setListenerAddressOptions(int fd);
std::string lastError();

#else

inline void init() {}
inline int close(int fd) { return ::close(fd); }
inline int shutdownBoth(int fd) { return ::shutdown(fd, SHUT_RDWR); }
inline ssize_t send(int fd, const void *data, size_t len) {
#ifdef MSG_NOSIGNAL
  constexpr int sendFlags = MSG_NOSIGNAL;
#else
  constexpr int sendFlags = 0;
#endif
  return ::send(fd, data, len, sendFlags);
}
inline ssize_t recv(int fd, void *data, size_t len) {
  return ::recv(fd, data, len, 0);
}
inline int dup(int fd) { return ::dup(fd); }
inline void setTimeoutSeconds(int fd, int seconds) {
  timeval timeout{};
  timeout.tv_sec = seconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}
inline void setListenerAddressOptions(int fd) {
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}
inline std::string lastError() { return std::strerror(errno); }

#endif

} // namespace sim_socket
