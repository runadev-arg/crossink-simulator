#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "SimFs.h"

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sim_http_fetch {

struct Response {
  int statusCode = 0;
  int curlExitCode = 0;
  std::string body;
  // Lowercased names, in receive order. With redirects followed, these are the
  // final response's headers.
  std::vector<std::pair<std::string, std::string>> headers;
  // The consumer stopped the transfer (onData returned false or shouldAbort
  // returned true).
  bool stoppedByData = false;
  bool stoppedByAbort = false;

  const std::string *header(const std::string &lowerName) const {
    for (const auto &entry : headers) {
      if (entry.first == lowerName)
        return &entry.second;
    }
    return nullptr;
  }
};

struct Request {
  std::string url;
  std::string method = "GET";
  std::map<std::string, std::string> headers;
  std::string basicAuth;
  // Not assumed to be a C string: a null `body` means "no body".
  const char *body = nullptr;
  size_t bodyLength = 0;
  bool insecureTls = false;
  // Negative follows redirects with curl's own limit.
  int maxRedirects = -1;
  // Transfer limit; 0 keeps the default of 60 seconds.
  uint32_t timeoutMs = 0;
  // When set, the body goes here rather than to Response::body, and the
  // response status and headers are already in Response when it is called.
  std::function<bool(const char *data, size_t length, const Response &partial)>
      onData;
  std::function<bool()> shouldAbort;
};

inline bool startsWith(const std::string &value, const char *prefix) {
  return value.rfind(prefix, 0) == 0;
}

inline std::string shellQuote(const std::string &value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

inline std::string urlDecode(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      int hi = hexValue(value[i + 1]);
      int lo = hexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

inline std::string basenameFromUrl(const std::string &url) {
  size_t end = url.find_first_of("?#");
  std::string path = url.substr(0, end == std::string::npos ? url.size() : end);
  size_t slash = path.find_last_of('/');
  return urlDecode(slash == std::string::npos ? path : path.substr(slash + 1));
}

inline bool readFile(const std::string &path, std::string &out) {
  FILE *file = sim_fs::fopen(path.c_str(), "rb");
  if (!file)
    return false;

  out.clear();
  std::array<char, 8192> buffer{};
  while (true) {
    size_t n = std::fread(buffer.data(), 1, buffer.size(), file);
    if (n > 0)
      out.append(buffer.data(), n);
    if (n < buffer.size()) {
      if (std::ferror(file)) {
        std::fclose(file);
        return false;
      }
      break;
    }
  }

  std::fclose(file);
  return true;
}

inline bool isDirectory(const std::string &path) {
  sim_fs::Stat st{};
  return sim_fs::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool findFileByBasename(const std::string &dir, const std::string &name,
                               std::string &outPath, int depth = 0) {
  if (depth > 5)
    return false;

  sim_fs::DIR *handle = sim_fs::opendir(dir.c_str());
  if (!handle)
    return false;

  while (sim_fs::dirent *entry = sim_fs::readdir(handle)) {
    std::string entryName = entry->d_name;
    if (entryName == "." || entryName == "..")
      continue;

    std::string path = dir;
    if (!path.empty() && path.back() != '/')
      path += '/';
    path += entryName;

    if (entryName == name) {
      outPath = path;
      sim_fs::closedir(handle);
      return true;
    }

    if (isDirectory(path) &&
        findFileByBasename(path, name, outPath, depth + 1)) {
      sim_fs::closedir(handle);
      return true;
    }
  }

  sim_fs::closedir(handle);
  return false;
}

inline bool fetchFromFileUrl(const std::string &url, Response &out) {
  if (!startsWith(url, "file://"))
    return false;

  std::string path = urlDecode(url.substr(strlen("file://")));
  if (startsWith(path, "localhost/"))
    path = path.substr(strlen("localhost"));
#ifdef _WIN32
  // file:///D:/books/a.epub decodes to /D:/books/a.epub.
  if (path.size() > 2 && path[0] == '/' && path[2] == ':')
    path.erase(0, 1);
#endif

  if (readFile(path, out.body)) {
    out.statusCode = 200;
  } else {
    out.body.clear();
    out.statusCode = 404;
  }
  return true;
}

inline bool fetchFromMockRoot(const std::string &url, Response &out) {
  const std::string root = sim_fs::getEnv("CROSSINK_SIM_HTTP_MOCK_ROOT");
  if (root.empty())
    return false;

  std::string name = basenameFromUrl(url);
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }

  std::string path = root;
  if (!path.empty() && path.back() != '/')
    path += '/';
  path += name;

  if (!readFile(path, out.body)) {
    std::string nestedPath;
    if (!findFileByBasename(root, name, nestedPath) ||
        !readFile(nestedPath, out.body))
      return false;
  }

  if (!out.body.empty()) {
    out.statusCode = 200;
    return true;
  }
  return false;
}

// Parses curl's --dump-header output. It holds one block per hop when redirects
// are followed, so only the last block describes the response we are returning.
inline void parseDumpedHeaders(const std::string &text, Response &out) {
  out.headers.clear();
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {
      out.headers.clear();
      const size_t space = line.find(' ');
      if (space != std::string::npos)
        out.statusCode = std::atoi(line.c_str() + space + 1);
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    size_t valueStart = colon + 1;
    while (valueStart < line.size() &&
           (line[valueStart] == ' ' || line[valueStart] == '\t'))
      ++valueStart;
    out.headers.emplace_back(std::move(name), line.substr(valueStart));
  }
}

#ifdef _WIN32

// SimHttpFetchWin.cpp: runs curl.exe directly, without a shell, keeping
// <windows.h> out of this header. The body arrives incrementally on a pipe.
bool fetchWithCurl(const Request &request, Response &out);

#else

inline bool fetchWithCurl(const Request &request, Response &out) {
  char bodyPath[] = "/tmp/crosspoint-sim-http-XXXXXX";
  int fd = mkstemp(bodyPath);
  if (fd < 0)
    return false;
  close(fd);
  char headerPath[] = "/tmp/crosspoint-sim-http-XXXXXX";
  fd = mkstemp(headerPath);
  if (fd < 0) {
    unlink(bodyPath);
    return false;
  }
  close(fd);

  std::string cmd = "curl";
  if (request.maxRedirects != 0)
    cmd += " -L";
  if (request.maxRedirects > 0)
    cmd += " --max-redirs " + std::to_string(request.maxRedirects);
  if (request.insecureTls)
    cmd += " -k";
  cmd += " -sS --connect-timeout 10 --max-time ";
  cmd += std::to_string(request.timeoutMs ? (request.timeoutMs + 999) / 1000
                                          : 60);
  cmd += " -o ";
  cmd += shellQuote(bodyPath);
  cmd += " -D ";
  cmd += shellQuote(headerPath);
  cmd += " -w '%{http_code}'";
  if (request.method != "GET")
    cmd += " -X " + shellQuote(request.method);
  for (const auto &header : request.headers) {
    cmd += " -H " + shellQuote(header.first + ": " + header.second);
  }
  if (!request.basicAuth.empty())
    cmd += " -u " + shellQuote(request.basicAuth);
  // A body passed on the command line stops at its first NUL byte.
  if (request.body)
    cmd += " --data-binary " +
           shellQuote(std::string(request.body, request.bodyLength));
  cmd += " " + shellQuote(request.url);

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    unlink(bodyPath);
    unlink(headerPath);
    return false;
  }

  std::string statusText;
  std::array<char, 64> statusBuffer{};
  while (
      fgets(statusBuffer.data(), static_cast<int>(statusBuffer.size()), pipe)) {
    statusText += statusBuffer.data();
  }
  const int rc = pclose(pipe);
  if (rc >= 0 && WIFEXITED(rc))
    out.curlExitCode = WEXITSTATUS(rc);

  bool readOk = readFile(bodyPath, out.body);
  unlink(bodyPath);
  if (!readOk)
    out.body.clear();

  std::string dumpedHeaders;
  if (readFile(headerPath, dumpedHeaders))
    parseDumpedHeaders(dumpedHeaders, out);
  unlink(headerPath);

  out.statusCode = std::atoi(statusText.c_str());
  return rc == 0 || out.statusCode > 0 || !out.body.empty();
}

#endif

// Hands an already complete body to a streaming consumer.
inline void deliverBody(const Request &request, Response &out) {
  if (!request.onData || out.body.empty())
    return;
  const std::string body = std::move(out.body);
  out.body.clear();
  constexpr size_t kChunk = 4096;
  for (size_t offset = 0; offset < body.size(); offset += kChunk) {
    if (request.shouldAbort && request.shouldAbort()) {
      out.stoppedByAbort = true;
      return;
    }
    const size_t length = std::min(kChunk, body.size() - offset);
    if (!request.onData(body.data() + offset, length, out)) {
      out.stoppedByData = true;
      return;
    }
  }
}

inline bool fetch(const Request &request, Response &out) {
  out = Response{};
  if (fetchFromMockRoot(request.url, out) ||
      fetchFromFileUrl(request.url, out)) {
    deliverBody(request, out);
    return true;
  }
  const bool ok = fetchWithCurl(request, out);
#ifndef _WIN32
  // The POSIX path buffers to a temp file first; Windows streams already.
  deliverBody(request, out);
#endif
  return ok;
}

inline bool fetch(const std::string &url, const char *method,
                  const std::map<std::string, std::string> &headers,
                  const std::string &basicAuth, const char *body,
                  Response &out) {
  Request request;
  request.url = url;
  if (method)
    request.method = method;
  request.headers = headers;
  request.basicAuth = basicAuth;
  request.body = body;
  request.bodyLength = body ? std::strlen(body) : 0;
  return fetch(request, out);
}

} // namespace sim_http_fetch
