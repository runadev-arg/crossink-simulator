#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "SimHttpFetch.h"

namespace sim_http_fetch {
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

std::string narrow(const std::wstring &wide) {
  if (wide.empty())
    return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

// One temp file, removed when the request ends. The directory comes from TMP.
class TempFile {
public:
  TempFile() {
    wchar_t directory[MAX_PATH + 1];
    wchar_t name[MAX_PATH + 1];
    if (GetTempPathW(MAX_PATH + 1, directory) &&
        GetTempFileNameW(directory, L"sim", 0, name))
      path_ = name;
  }
  ~TempFile() {
    if (!path_.empty())
      DeleteFileW(path_.c_str());
  }
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;

  bool valid() const { return !path_.empty(); }
  std::string utf8Path() const { return narrow(path_); }

  bool write(const char *data, size_t length) const {
    FILE *file = _wfopen(path_.c_str(), L"wb");
    if (!file)
      return false;
    const bool ok = length == 0 || fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
  }

private:
  std::wstring path_;
};

// Quoted value for a curl config file.
std::string configValue(const std::string &value) {
  std::string out = "\"";
  for (const char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out += c;
      break;
    }
  }
  out += '"';
  return out;
}

void addOption(std::string &config, const char *name,
               const std::string &value) {
  config += name;
  config += " = ";
  config += configValue(value);
  config += '\n';
}

// Holding the spawn lock while the write end of the stdout pipe exists in this
// process stops a concurrent request's curl inheriting it: that curl would keep
// the pipe open and this reader would not see end-of-file until it exited.
std::mutex &spawnMutex() {
  static std::mutex mutex;
  return mutex;
}

struct CurlProcess {
  HANDLE process = nullptr;
  HANDLE output = nullptr;

  ~CurlProcess() {
    if (output)
      CloseHandle(output);
    if (process)
      CloseHandle(process);
  }
};

bool startCurl(const std::string &configPath, CurlProcess &curl) {
  // Options travel in a config file, read as raw bytes: the command line would be
  // re-encoded to the ANSI code page by curl's narrow main(), mangling non-ASCII
  // headers and URLs, and it would also show credentials to anything listing
  // processes.
  std::wstring commandLine = L"curl.exe --config \"" + widen(configPath) + L"\"";

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;

  std::lock_guard<std::mutex> lock(spawnMutex());

  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 256 * 1024))
    return false;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &inheritable, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul;
  startup.hStdOutput = writeEnd;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION info{};
  const BOOL started =
      CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  const DWORD spawnError = GetLastError();
  CloseHandle(writeEnd);
  if (nul != INVALID_HANDLE_VALUE)
    CloseHandle(nul);
  if (!started) {
    CloseHandle(readEnd);
    std::fprintf(stderr,
                 "[SIM] Cannot start curl.exe (error %lu). HTTP needs curl on "
                 "PATH; Windows 10 and later ship it in System32.\n",
                 spawnError);
    return false;
  }
  CloseHandle(info.hThread);
  curl.process = info.hProcess;
  curl.output = readEnd;
  return true;
}

} // namespace

bool fetchWithCurl(const Request &request, Response &out) {
  TempFile configFile;
  TempFile headerFile;
  TempFile bodyFile;
  if (!configFile.valid() || !headerFile.valid() || !bodyFile.valid()) {
    std::fprintf(stderr, "[SIM] Cannot create HTTP temp files\n");
    return false;
  }

  // globoff keeps literal []{} in URLs. HTTP/1.1 and no Expect header match the
  // device's client.
  std::string config = "silent\nshow-error\ngloboff\nhttp1.1\nno-buffer\n";
  config += "connect-timeout = 10\n";
  if (request.onData) {
    // A stream has no natural upper bound; give up when it stalls instead.
    const uint32_t idleSeconds =
        request.timeoutMs ? (request.timeoutMs + 999) / 1000 : 60;
    config += "speed-limit = 1\nspeed-time = " + std::to_string(idleSeconds) +
              "\n";
  } else {
    const uint32_t maxSeconds =
        request.timeoutMs ? (request.timeoutMs + 999) / 1000 : 60;
    config += "max-time = " + std::to_string(maxSeconds) + "\n";
  }
  if (request.maxRedirects != 0)
    config += "location\n";
  if (request.maxRedirects > 0)
    config += "max-redirs = " + std::to_string(request.maxRedirects) + "\n";
  if (request.insecureTls)
    config += "insecure\n";
  // Loopback test servers must not go through a proxy set in the environment.
  addOption(config, "noproxy", "127.0.0.1,localhost");
  addOption(config, "dump-header", headerFile.utf8Path());
  if (request.method != "GET")
    addOption(config, "request", request.method);
  for (const auto &header : request.headers)
    addOption(config, "header", header.first + ": " + header.second);
  addOption(config, "header", "Expect:");
  if (!request.basicAuth.empty())
    addOption(config, "user", request.basicAuth);
  if (request.body) {
    if (!bodyFile.write(request.body, request.bodyLength)) {
      std::fprintf(stderr, "[SIM] Cannot write HTTP request body\n");
      return false;
    }
    addOption(config, "data-binary", "@" + bodyFile.utf8Path());
  }
  addOption(config, "url", request.url);
  if (!configFile.write(config.data(), config.size())) {
    std::fprintf(stderr, "[SIM] Cannot write curl config\n");
    return false;
  }

  CurlProcess curl;
  if (!startCurl(configFile.utf8Path(), curl))
    return false;

  bool headersLoaded = false;
  const auto loadHeaders = [&] {
    std::string dumped;
    if (readFile(headerFile.utf8Path(), dumped))
      parseDumpedHeaders(dumped, out);
    headersLoaded = true;
  };

  std::vector<char> buffer(64 * 1024);
  bool stopped = false;
  bool exited = false;
  while (!stopped) {
    DWORD available = 0;
    const bool pipeOpen =
        PeekNamedPipe(curl.output, nullptr, 0, nullptr, &available, nullptr) !=
        0;
    if (!pipeOpen)
      break;
    if (available == 0) {
      if (exited)
        break; // Drained after the process ended.
      if (request.shouldAbort && request.shouldAbort()) {
        out.stoppedByAbort = true;
        stopped = true;
        break;
      }
      exited = WaitForSingleObject(curl.process, 5) == WAIT_OBJECT_0;
      continue;
    }
    DWORD got = 0;
    if (!ReadFile(curl.output, buffer.data(),
                  static_cast<DWORD>(buffer.size()), &got, nullptr) ||
        got == 0)
      break;
    if (!request.onData) {
      out.body.append(buffer.data(), got);
      continue;
    }
    // curl flushes the dumped headers before it writes any body byte.
    if (!headersLoaded)
      loadHeaders();
    if (request.shouldAbort && request.shouldAbort()) {
      out.stoppedByAbort = true;
      stopped = true;
    } else if (!request.onData(buffer.data(), got, out)) {
      out.stoppedByData = true;
      stopped = true;
    }
  }

  if (stopped)
    TerminateProcess(curl.process, 1);
  WaitForSingleObject(curl.process, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(curl.process, &exitCode);
  out.curlExitCode = static_cast<int>(exitCode);
  loadHeaders();
  return exitCode == 0 || out.statusCode > 0 || !out.body.empty();
}

} // namespace sim_http_fetch

#endif // _WIN32
