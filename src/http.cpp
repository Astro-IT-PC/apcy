#include "http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace {

size_t writeBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t writeHeader(char *buffer, size_t size, size_t nitems, void *userdata) {
    auto *resp = static_cast<HttpResponse *>(userdata);
    std::string line(buffer, size * nitems);
    const std::string key = "retry-after:";
    if (line.size() > key.size()) {
        std::string lower;
        lower.reserve(key.size());
        for (size_t i = 0; i < key.size(); ++i) lower.push_back((char)std::tolower((unsigned char)line[i]));
        if (lower == key) {
            std::string v = line.substr(key.size());
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            resp->retryAfter = v;
        }
    }
    return size * nitems;
}

}  // namespace

void HttpClient::globalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void HttpClient::globalCleanup() { curl_global_cleanup(); }

HttpClient::HttpClient() {
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
}

HttpClient::~HttpClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL *>(curl_));
}

HttpResponse HttpClient::request(const std::string &method, const std::string &url,
                                 const std::vector<std::string> &headers, const std::string &body) {
    CURL *curl = static_cast<CURL *>(curl_);
    HttpResponse resp;

    struct curl_slist *list = nullptr;
    for (const auto &h : headers) list = curl_slist_append(list, h.c_str());

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifndef APCY_VERSION
#define APCY_VERSION "dev"
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "apcy/" APCY_VERSION " (clay+raylib)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_slist_free_all(list);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc));
    }
    return resp;
}

std::string HttpClient::urlEncode(const std::string &value) {
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}
