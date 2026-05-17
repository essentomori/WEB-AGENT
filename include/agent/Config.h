#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <filesystem>

namespace agent {

struct ServerConfig {
    std::string base_url;
    std::string api_path;
    std::string registration_endpoint;
    std::string task_endpoint;
    std::string result_endpoint;
    int connection_timeout_sec = 30;
    int request_timeout_sec = 60;
    bool verify_ssl = true;
    std::string ca_bundle_path;
};

struct PollingConfig {
    int base_interval_sec = 60;
    int max_interval_sec = 3600;
    double backoff_multiplier = 2.0;
    int max_retries = 3;
    int registration_retries = 3;
    int result_retries = 3;
};

struct ExecutionConfig {
    std::filesystem::path task_dir = "tasks";
    std::filesystem::path result_dir = "results";
    std::filesystem::path temp_dir = "tmp";
    std::filesystem::path log_dir = "logs";
    int process_timeout_sec = 300;
    int file_upload_timeout_sec = 120;
    int max_output_size_bytes = 10 * 1024 * 1024; // 10MB
    std::vector<std::string> allowed_commands;
    bool sandbox_mode = true;
};

struct LoggingConfig {
    enum class Level { TRACE, DEBUG, INFO, WARNING, ERROR, CRITICAL };
    Level console_level = Level::INFO;
    Level file_level = Level::DEBUG;
    std::filesystem::path log_file = "logs/agent.log";
    size_t max_file_size_mb = 100;
    int max_files_rotation = 5;
    bool structured_json = false;
};

struct AgentIdentity {
    std::string uid;
    std::string description;
    std::optional<std::string> access_code;
    std::string version = "2.0.0";
};

class Config {
public:
    static Config& getInstance();
    
    bool loadFromFile(const std::filesystem::path& path);
    bool loadFromEnv();
    bool validate() const;
    
    // Getters
    const ServerConfig& getServer() const { return server_; }
    const PollingConfig& getPolling() const { return polling_; }
    const ExecutionConfig& getExecution() const { return execution_; }
    const LoggingConfig& getLogging() const { return logging_; }
    const AgentIdentity& getIdentity() const { return identity_; }
    
    // Setters with validation
    void setPollingInterval(int seconds);
    void setAccessCode(const std::string& code);
    
private:
    Config() = default;
    
    ServerConfig server_;
    PollingConfig polling_;
    ExecutionConfig execution_;
    LoggingConfig logging_;
    AgentIdentity identity_;
    
    bool validateUrl(const std::string& url) const;
    bool validatePath(const std::filesystem::path& path) const;
};

} // namespace agent
