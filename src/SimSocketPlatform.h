#pragma once

// System socket headers (sockaddr_in, getaddrinfo, htons, ...). Include this
// AFTER the firmware headers of a .cpp, never from a header: on Windows it brings
// in <windows.h>, whose macros collide with firmware identifiers.

#include "SimSocket.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI // wingdi.h defines ERROR
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#undef DELETE
#undef IN
#undef OUT
#undef OPTIONAL
#undef ERROR
#undef interface
#undef near
#undef far

#else

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#endif
