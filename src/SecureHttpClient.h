#pragma once

#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "HTTPClient.h"
#include "NetworkClient.h"
#include "SimHttpFetch.h"
#include "WString.h"

namespace freeink {

// Mirrors the public surface of the SDK's SecureHttpClient, so firmware code
// written against it also runs here. TLS is curl's, not wolfSSL: this does not
// exercise device certificate or memory behaviour.
class SecureHttpClient {
public:
  using DataCallback = std::function<bool(const uint8_t *data, size_t len)>;
  using AbortCallback = std::function<bool()>;
  using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;

  void setCACert(const char *) {}
  void setInsecure() { insecure_ = true; }
  void setTimeout(uint32_t ms) { timeoutMs_ = ms; }
  void setProgressCallback(const ProgressCallback &progress) {
    progress_ = progress;
  }
  void setFollowRedirects(int maxHops) {
    followRedirects_ = maxHops < 0 ? 0 : maxHops;
  }
  void setAllowRedirectDowngrade(bool) {}
  void setBasicAuth(const std::string &user, const std::string &pass) {
    basicAuth_ = user + ":" + pass;
  }
  void clearBasicAuth() { basicAuth_.clear(); }
  void setUserAgent(const std::string &ua) { headers_["User-Agent"] = ua; }
  void setReuse(bool) {}

  bool begin(const String &url) { return begin(url.s); }
  bool begin(const char *url) { return begin(std::string(url ? url : "")); }
  bool begin(const std::string &url) {
    url_ = url;
    resetResponse();
    return !url_.empty();
  }

  void end() {
    url_.clear();
    headers_.clear();
    basicAuth_.clear();
    resetResponse();
  }

  void addHeader(const char *name, const String &value) {
    addHeader(std::string(name ? name : ""), value.s);
  }
  void addHeader(const char *name, const char *value) {
    addHeader(std::string(name ? name : ""), std::string(value ? value : ""));
  }
  void addHeader(const std::string &name, const std::string &value) {
    if (!name.empty())
      headers_[name] = value;
  }

  int GET() { return perform("GET", nullptr, 0, nullptr, nullptr); }
  int GET(const DataCallback &onData, const AbortCallback &shouldAbort = nullptr) {
    return perform("GET", nullptr, 0, onData, shouldAbort);
  }
  int POST(const String &payload) { return sendRequest("POST", payload.s); }
  int POST(const std::string &payload) { return sendRequest("POST", payload); }
  int POST(const char *payload) {
    return sendRequest("POST", std::string(payload ? payload : ""));
  }
  int sendRequest(const char *method, const String &payload) {
    return sendRequest(method, payload.s);
  }
  int sendRequest(const char *method, const std::string &payload) {
    return perform(method, payload.data(), payload.size(), nullptr, nullptr);
  }
  int sendRequest(const char *method, const uint8_t *payload, size_t payloadLen) {
    return perform(method, reinterpret_cast<const char *>(payload), payloadLen,
                   nullptr, nullptr);
  }
  int sendRequest(const char *method, const uint8_t *payload, size_t payloadLen,
                  const DataCallback &onData,
                  const AbortCallback &shouldAbort = nullptr) {
    return perform(method, reinterpret_cast<const char *>(payload), payloadLen,
                   onData, shouldAbort);
  }

  String getString() { return String(response_.body); }
  int getStatus() const { return response_.statusCode; }
  int getSize() { return static_cast<int>(response_.body.size()); }
  bool responseComplete() const { return complete_; }
  bool callbackAborted() const { return response_.stoppedByData; }
  bool aborted() const { return response_.stoppedByAbort; }
  bool hasContentLength() const {
    return response_.header("content-length") != nullptr;
  }
  size_t getContentLength() const {
    const std::string *value = response_.header("content-length");
    return value ? static_cast<size_t>(std::strtoull(value->c_str(), nullptr, 10))
                 : 0;
  }
  std::string getHeader(const std::string &headerName) const {
    std::string normalized = headerName;
    for (char &c : normalized)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string *value = response_.header(normalized);
    return value ? *value : std::string();
  }
  std::vector<std::pair<std::string, std::string>> getHeaders() const {
    return response_.headers;
  }

  static bool tls13Available() { return true; }

private:
  void resetResponse() {
    response_ = sim_http_fetch::Response{};
    complete_ = false;
  }

  int perform(const char *method, const char *payload, size_t payloadLen,
              const DataCallback &onData, const AbortCallback &shouldAbort) {
    if (url_.empty())
      return -1;

    sim_http_fetch::Request request;
    request.url = url_;
    request.method = method ? method : "GET";
    request.headers = headers_;
    request.basicAuth = basicAuth_;
    request.body = payload;
    request.bodyLength = payloadLen;
    request.insecureTls = insecure_;
    request.maxRedirects = followRedirects_;
    request.timeoutMs = timeoutMs_;
    size_t delivered = 0;
    if (onData || progress_) {
      request.onData = [&](const char *data, size_t length,
                           const sim_http_fetch::Response &partial) {
        delivered += length;
        const std::string *total = partial.header("content-length");
        if (progress_ &&
            !progress_(delivered,
                       total ? static_cast<size_t>(
                                   std::strtoull(total->c_str(), nullptr, 10))
                             : 0))
          return false;
        if (onData) {
          // getStatus() and getHeader() are valid inside the device's callback.
          response_.statusCode = partial.statusCode;
          response_.headers = partial.headers;
          return onData(reinterpret_cast<const uint8_t *>(data), length);
        }
        return true;
      };
    }
    request.shouldAbort = shouldAbort;

    resetResponse();
    sim_http_fetch::Response result;
    const bool ok = sim_http_fetch::fetch(request, result);
    response_ = std::move(result);
    complete_ = ok && response_.curlExitCode == 0 &&
                !response_.stoppedByAbort && !response_.stoppedByData;
    if (!ok || response_.statusCode <= 0)
      return -1;
    return response_.statusCode;
  }

  std::string url_;
  std::map<std::string, std::string> headers_;
  std::string basicAuth_;
  bool insecure_ = false;
  // The SDK client does not follow redirects unless asked to.
  int followRedirects_ = 0;
  uint32_t timeoutMs_ = 0;
  ProgressCallback progress_;
  sim_http_fetch::Response response_;
  bool complete_ = false;
};

} // namespace freeink
