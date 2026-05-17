#include "agent/Config.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <cstdlib>

namespace agent {

using json = nlohmann::json;

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

bool Config::loadFromFile(const std::filesystem::path& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;
        }
        
        json data = json::parse(file);
        
        // Server config
        if (data.contains("server")) {
            auto& srv = data["server"];
            if (srv.contains("base_url")) server_.base_url = srv["base_url"];
            if (srv.contains("api_path")) server_.api_path = srv["api_path"];
            if (srv.contains("connection_timeout")) server_.connection_timeout_sec = srv["connection_timeout"];
            if (srv.contains("verify_ssl")) server_.verify_ssl = srv["verify_ssl"];
        }
        
        // Polling config
        if (data.contains("polling")) {
            auto& poll = data["polling"];
            if (poll.contains("base_interval")) polling_.base_interval_sec = poll["base_interval"];
            if (poll.contains("max_interval")) polling_.max_interval_sec = poll["max_interval"];
            if (poll.contains("backoff_multiplier")) polling_.backoff_multiplier = poll["backoff_multiplier"];
        }
        
        // Execution config
        if (data.contains("execution")) {
            auto& exec = data["execution"];
            if (exec.contains("task_dir")) execution_.task_dir = exec["task_dir"].get<std::string>();
            if (exec.contains("result_dir")) execution_.result_dir = exec["result_dir"].get<std::string>();
            if (exec.contains("process_timeout")) execution_.process_timeout_sec = exec["process_timeout"];
        }
        
        // Identity
        if (data.contains("identity")) {
            auto& id = data["identity"];
            if (id.contains("uid")) identity_.uid = id["uid"];
            if (id.contains("description")) identity_.description = id["description"];
            if (id.contains("access_code")) identity_.access_code = id["access_code"];
        }
        
        return validate();
        
    } catch (const std::exception& e) {
        return false;
    }
}

bool Config::loadFromEnv() {
    const char* env_url = std::getenv("AGENT_SERVER_URL");
    if (env_url) server_.base_url = env_url;
    
    const char* env_uid = std::getenv("AGENT_UID");
    if (env_uid) identity_.uid = env_uid;
    
    const char* env_code = std::getenv("AGENT_ACCESS_CODE");
    if (env_code) identity_.access_code = env_code;
    
    return validate();
}

bool Config::validate() const {
    // Validate URLs
    if (server_.base_url.empty()) return false;
    if (!validateUrl(server_.base_url)) return false;
    
    // Validate paths
    if (!validatePath(execution_.task_dir)) return false;
    if (!validatePath(execution_.result_dir)) return false;
    if (!validatePath(execution_.log_dir)) return false;
    
    // Validate intervals
    if (polling_.base_interval_sec <= 0) return false;
    if (polling_.max_interval_sec < polling_.base_interval_sec) return false;
    
    // Validate identity
    if (identity_.uid.empty()) return false;
    
    return true;
}

void Config::setPollingInterval(int seconds) {
    if (seconds > 0) {
        polling_.base_interval_sec = seconds;
        if (polling_.max_interval_sec < seconds) {
            polling_.max_interval_sec = seconds;
        }
    }
}

bool Config::validateUrl(const std::string& url) const {
    return url.find("http://") == 0 || url.find("https://") == 0;
}

bool Config::validatePath(const std::filesystem::path& path) const {
    // Create directory if needed
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path, ec);
    }
    return !ec;
}

} // namespace agent
