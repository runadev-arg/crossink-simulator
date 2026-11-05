// Drives the simulator's HTTP clients and storage shim against a real local
// server. Run through tests/run_http_stack_check.py, which owns the server.
//
// Usage: http_stack_check <base-url> <refused-url>

#include <HTTPClient.h>
#include <HalStorage.h>
#include <MD5Builder.h>
#include <SecureHttpClient.h>
#include <esp_http_client.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(const char *name, bool ok, const std::string &detail = "") {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
              detail.empty() ? "" : " :: ", detail.c_str());
  std::fflush(stdout);
  if (!ok)
    ++failures;
}

std::string md5Of(const std::string &data) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  md5.calculate();
  return md5.toString().s;
}

std::string md5OfSdFile(const char *path) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file)
    return "<cannot open>";
  MD5Builder md5;
  md5.begin();
  std::vector<uint8_t> buffer(8192);
  int got;
  while ((got = file.read(buffer.data(), buffer.size())) > 0)
    md5.add(buffer.data(), static_cast<size_t>(got));
  md5.calculate();
  return md5.toString().s;
}

// CRLF, NUL and Ctrl-Z all break a text-mode or C-string code path.
std::string binaryBody() {
  std::string body = "line one\r\nline two\n";
  body.push_back('\0');
  body.append("after nul ");
  body.push_back('\x1a');
  body.append(" after ctrl-z ");
  for (int i = 0; i < 256; ++i)
    body.push_back(static_cast<char>(i));
  return body;
}

void checkMd5Vectors() {
  check("md5 empty", md5Of("") == "d41d8cd98f00b204e9800998ecf8427e");
  check("md5 abc", md5Of("abc") == "900150983cd24fb0d6963f7d28e17f72");
  check("md5 message digest",
        md5Of("12345678901234567890123456789012345678901234567890123456789012"
              "345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
  check("md5 million a", md5Of(std::string(1000000, 'a')) ==
                             "7707d6ae4e027c70eea2a935c2296f21");
}

void checkStorage() {
  check("storage begin", Storage.begin());

  const std::string body = binaryBody();
  {
    HalFile out = Storage.open("/check/binary.bin", O_WRONLY | O_CREAT | O_TRUNC);
    check("storage create nested", out.isOpen());
    check("storage write all",
          out.write(body.data(), body.size()) == body.size());
    out.close();
  }
  {
    HalFile in = Storage.open("/check/binary.bin", O_RDONLY);
    std::string read(body.size() + 16, '\0');
    const int got = in.read(read.data(), read.size());
    read.resize(got > 0 ? static_cast<size_t>(got) : 0);
    check("storage binary round trip", read == body);
    check("storage size is exact", in.size() == body.size());
  }
  {
    HalFile append = Storage.open("/check/binary.bin", O_WRONLY | O_APPEND);
    append.write(reinterpret_cast<const uint8_t *>("ZZ"), 2);
    append.close();
    HalFile in = Storage.open("/check/binary.bin", O_RDONLY);
    check("storage append", in.size() == body.size() + 2);
    in.seek(body.size());
    check("storage seek", in.read() == 'Z');
  }

  Storage.writeFile("/check/old.txt", String("old"));
  Storage.writeFile("/check/new.txt", String("new"));
  check("storage rename replaces target",
        Storage.rename("/check/new.txt", "/check/old.txt") &&
            Storage.readFile("/check/old.txt").s == "new" &&
            !Storage.exists("/check/new.txt"));

  const char *utf8Path = "/check/Espa\xc3\xb1ol \xe6\x9c\xac.txt";
  check("storage utf-8 write", Storage.writeFile(utf8Path, String("utf8")));
  check("storage utf-8 read", Storage.readFile(utf8Path).s == "utf8");
  bool listed = false;
  {
    HalFile dir = Storage.open("/check");
    check("storage opens directory", dir.isDirectory());
    while (true) {
      HalFile child = dir.openNextFile();
      if (!child)
        break;
      char name[256];
      child.getName(name, sizeof(name));
      if (std::string(name) == "Espa\xc3\xb1ol \xe6\x9c\xac.txt")
        listed = true;
    }
    dir.close();
  }
  check("storage lists utf-8 name", listed);

  {
    HalFile open = Storage.open("/check/held.txt", O_WRONLY | O_CREAT);
    check("storage removes open file", Storage.remove("/check/held.txt"));
    open.close();
  }
  check("storage rejects traversal", !Storage.exists("/../check") &&
                                         !Storage.exists("/..\\check"));
  check("storage rmdir recursive",
        Storage.removeDir("/check") && !Storage.exists("/check"));
}

void checkArduinoHttpClient(const std::string &base) {
  NetworkClient client;
  HTTPClient http;

  http.begin(client, (base + "/hello").c_str());
  check("HTTPClient GET status", http.GET() == 200);
  check("HTTPClient GET body", http.getString().s == "hello world");
  check("HTTPClient GET header", http.header("X-Hello").s == "yes");
  http.end();

  const std::string body = binaryBody();
  http.begin(client, (base + "/echo-md5").c_str());
  http.addHeader("X-Custom", "caf\xc3\xa9 tok\"en");
  http.addHeader("Content-Type", "application/octet-stream");
  const int posted = http.POST(reinterpret_cast<const uint8_t *>(body.data()),
                               body.size());
  check("HTTPClient POST binary", posted == 200);
  check("HTTPClient POST body arrived intact",
        http.getString().s == "POST " + md5Of(body) + " " +
                                  std::to_string(body.size()) +
                                  " caf\xc3\xa9 tok\"en",
        http.getString().s);
  http.end();

  http.begin(client, (base + "/echo-md5").c_str());
  check("HTTPClient PUT String", http.PUT(String(body)) == 200);
  check("HTTPClient PUT keeps NUL",
        http.getString().s.rfind("PUT " + md5Of(body), 0) == 0,
        http.getString().s);
  http.end();

  http.begin(client, (base + "/missing").c_str());
  check("HTTPClient 404", http.GET() == 404);
  http.end();

  HTTPClient auth;
  auth.begin(client, (base + "/auth").c_str());
  auth.setAuthorization("toto", "s3cret");
  check("HTTPClient basic auth", auth.GET() == 200);
  check("HTTPClient basic auth value",
        auth.getString().s == "Basic dG90bzpzM2NyZXQ=", auth.getString().s);
  auth.end();
}

void checkRefused(const std::string &refused) {
  NetworkClient client;
  HTTPClient http;
  http.begin(client, (refused + "/nothing").c_str());
  const int status = http.GET();
  check("HTTPClient refused maps to error", status == HTTPC_ERROR_CONNECTION_REFUSED,
        std::to_string(status));
  http.end();

  freeink::SecureHttpClient secure;
  secure.begin(refused + "/nothing");
  check("SecureHttpClient refused is -1", secure.GET() == -1);
  check("SecureHttpClient refused is incomplete", !secure.responseComplete());
}

void checkSecureClient(const std::string &base) {
  const std::string body = binaryBody();
  freeink::SecureHttpClient http;

  http.begin(base + "/echo-md5");
  http.setUserAgent("sim-check/1");
  http.addHeader("X-Custom", "secure");
  check("SecureHttpClient PUT",
        http.sendRequest("PUT", reinterpret_cast<const uint8_t *>(body.data()),
                         body.size()) == 200);
  check("SecureHttpClient PUT body intact",
        http.getString().s == "PUT " + md5Of(body) + " " +
                                  std::to_string(body.size()) + " secure",
        http.getString().s);
  check("SecureHttpClient user agent",
        http.getHeader("X-Seen-User-Agent") == "sim-check/1");
  http.end();

  http.begin(base + "/echo-md5");
  http.addHeader("X-Custom", "del");
  check("SecureHttpClient DELETE", http.sendRequest("DELETE", std::string()) == 200);
  check("SecureHttpClient DELETE method",
        http.getString().s.rfind("DELETE ", 0) == 0, http.getString().s);
  http.end();

  http.begin(base + "/redirect");
  check("SecureHttpClient does not follow by default", http.GET() == 302);
  check("SecureHttpClient exposes Location",
        http.getHeader("location") == "/hello");
  http.end();

  http.begin(base + "/redirect");
  http.setFollowRedirects(3);
  check("SecureHttpClient follows when asked", http.GET() == 200);
  check("SecureHttpClient final body", http.getString().s == "hello world");
  check("SecureHttpClient complete", http.responseComplete());
  http.end();
}

void checkStreamingDownload(const std::string &base) {
  using Clock = std::chrono::steady_clock;

  HalFile out = Storage.open("/dl/stream.bin", O_WRONLY | O_CREAT | O_TRUNC);
  check("stream opens destination", out.isOpen());

  freeink::SecureHttpClient http;
  http.begin(base + "/big?pace=1");
  http.setUserAgent("sim-check/1");
  size_t received = 0;
  int callbacks = 0;
  int statusInCallback = 0;
  size_t lengthInCallback = 0;
  std::string md5Header;
  const auto started = Clock::now();
  long firstMs = -1;
  long lastMs = -1;
  const int status = http.GET(
      [&](const uint8_t *data, size_t length) {
        const long now = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                  started)
                .count());
        if (firstMs < 0)
          firstMs = now;
        lastMs = now;
        if (callbacks == 0) {
          statusInCallback = http.getStatus();
          lengthInCallback = http.getContentLength();
          md5Header = http.getHeader("X-Content-MD5");
        }
        ++callbacks;
        received += length;
        return out.write(data, length) == length;
      });
  out.sync();
  out.close();

  check("stream status", status == 200, std::to_string(status));
  check("stream complete", http.responseComplete());
  check("stream status known in callback", statusInCallback == 200);
  check("stream length known in callback",
        lengthInCallback == received && received == 5 * 1024 * 1024,
        std::to_string(lengthInCallback) + "/" + std::to_string(received));
  // The server paces the body over about a second: a client that only delivers
  // once the transfer is over would squeeze every callback into the same instant.
  check("stream arrives incrementally",
        callbacks >= 5 && lastMs - firstMs >= 500,
        std::to_string(callbacks) + " callbacks over " +
            std::to_string(lastMs - firstMs) + " ms");
  check("stream file matches", !md5Header.empty() &&
                                   md5OfSdFile("/dl/stream.bin") == md5Header,
        md5Header);
  http.end();

  size_t beforeAbort = 0;
  freeink::SecureHttpClient cancelled;
  cancelled.begin(base + "/big?pace=1");
  cancelled.GET(
      [&](const uint8_t *, size_t length) {
        beforeAbort += length;
        return true;
      },
      [&] { return beforeAbort > 0; });
  check("stream abort stops transfer",
        cancelled.aborted() && !cancelled.responseComplete() &&
            beforeAbort < 5 * 1024 * 1024,
        std::to_string(beforeAbort));
  cancelled.end();
}

// The sequence firmware/src/network/HttpDownloader.cpp uses.
void checkEspClientDownload(const std::string &base) {
  std::string url = base + "/big";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  std::string md5Header;
  config.user_data = &md5Header;
  config.event_handler = [](esp_http_client_event_t *event) -> esp_err_t {
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(event->header_key, "X-Content-MD5") == 0)
      static_cast<std::string *>(event->user_data)
          ->assign(event->header_value);
    return ESP_OK;
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Connection", "close");
  check("esp open", esp_http_client_open(client, 0) == ESP_OK);
  const int64_t length = esp_http_client_fetch_headers(client);
  check("esp length", length == 5 * 1024 * 1024);
  check("esp status", esp_http_client_get_status_code(client) == 200);

  HalFile out = Storage.open("/dl/esp.bin", O_WRONLY | O_CREAT | O_TRUNC);
  std::vector<char> buffer(2048);
  int got;
  while ((got = esp_http_client_read(client, buffer.data(),
                                     static_cast<int>(buffer.size()))) > 0)
    out.write(buffer.data(), static_cast<size_t>(got));
  out.close();
  check("esp complete", esp_http_client_is_complete_data_received(client));
  esp_http_client_cleanup(client);
  check("esp file matches",
        !md5Header.empty() && md5OfSdFile("/dl/esp.bin") == md5Header, md5Header);

  // Resume with Range, as HttpDownloader's resumePartial does.
  HalFile cut = Storage.open("/dl/esp.bin", O_RDONLY);
  std::string head(1024 * 1024, '\0');
  cut.read(head.data(), head.size());
  cut.close();
  HalFile restart = Storage.open("/dl/resume.bin", O_WRONLY | O_CREAT | O_TRUNC);
  restart.write(head.data(), head.size());
  restart.close();

  esp_http_client_config_t resume = {};
  resume.url = url.c_str();
  client = esp_http_client_init(&resume);
  esp_http_client_set_header(client, "Range", "bytes=1048576-");
  esp_http_client_open(client, 0);
  esp_http_client_fetch_headers(client);
  check("esp range status", esp_http_client_get_status_code(client) == 206);
  out = Storage.open("/dl/resume.bin", O_WRONLY | O_APPEND);
  while ((got = esp_http_client_read(client, buffer.data(),
                                     static_cast<int>(buffer.size()))) > 0)
    out.write(buffer.data(), static_cast<size_t>(got));
  out.close();
  esp_http_client_cleanup(client);
  check("esp resumed file matches", md5OfSdFile("/dl/resume.bin") == md5Header);

  // POST with the body written after open(), and a response read back.
  const std::string body = binaryBody();
  std::string echoUrl = base + "/echo-md5";
  esp_http_client_config_t post = {};
  post.url = echoUrl.c_str();
  post.method = HTTP_METHOD_POST;
  client = esp_http_client_init(&post);
  esp_http_client_set_header(client, "X-Custom", "esp");
  check("esp open for write",
        esp_http_client_open(client, static_cast<int>(body.size())) == ESP_OK);
  check("esp write", esp_http_client_write(client, body.data(),
                                           static_cast<int>(body.size())) ==
                         static_cast<int>(body.size()));
  esp_http_client_fetch_headers(client);
  check("esp POST status", esp_http_client_get_status_code(client) == 200);
  std::string reply(512, '\0');
  got = esp_http_client_read(client, reply.data(),
                             static_cast<int>(reply.size()));
  reply.resize(got > 0 ? static_cast<size_t>(got) : 0);
  check("esp POST body intact",
        reply == "POST " + md5Of(body) + " " + std::to_string(body.size()) +
                     " esp",
        reply);
  esp_http_client_cleanup(client);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <base-url> <refused-url>\n", argv[0]);
    return 2;
  }
  const std::string base = argv[1];

  checkMd5Vectors();
  checkStorage();
  checkArduinoHttpClient(base);
  checkRefused(argv[2]);
  checkSecureClient(base);
  checkStreamingDownload(base);
  checkEspClientDownload(base);

  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
