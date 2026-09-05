// Minimal blocking HTTP client on top of libcurl.
// Deliberately does not include curl headers so that on Windows this header
// can coexist with raylib.h (curl pulls in windows.h).
#pragma once

#include <string>
#include <vector>

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string retryAfter;  // value of the Retry-After header, if present
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    // method: "GET", "PUT", "POST", ... body is sent as-is for non-GET requests.
    HttpResponse request(const std::string &method, const std::string &url,
                         const std::vector<std::string> &headers, const std::string &body = "");
    HttpResponse get(const std::string &url, const std::vector<std::string> &headers) {
        return request("GET", url, headers);
    }

    static std::string urlEncode(const std::string &value);
    static void globalInit();
    static void globalCleanup();

private:
    void *curl_ = nullptr;  // CURL*
};
