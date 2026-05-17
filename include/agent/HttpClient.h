#pragma once

#include <string>
#include <optional>
#include <functional>
#include <chrono>

namespace agent {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string error_message;
    std::chrono::milliseconds response_time{0};
    
    bool success() const { return status_code >= 200 && status_code < 300; }
};

class HttpClient {
public:
    using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
    
    explicit HttpClient(const ServerConfig& config);
    ~HttpClient();
    
    // POST with automatic retry and backoff
    HttpResponse post(const std::string& endpoint,
                     const std::string& body,
                     const std::string& content_type = "application/json",
                     int retry_count = 0);
    
    // Multipart POST for file upload
    HttpResponse postMultipart(const std::string& endpoint,
                              const std::map<std::string, std::string>& fields,
                              const std::map<std::string, std::string>& files,
                              int retry_count = 0);
    
    void setTimeout(std::chrono::seconds timeout);
    void setProgressCallback(ProgressCallback callback);
    
private:
    struct Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace agent
