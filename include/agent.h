#pragma once

#include <string>
#include <sstream>
#include <optional>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <functional>
#include <map>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>

inline constexpr const char* CONFIG_FILE_PATH = "config.json";

// Глобальный флаг для Graceful Shutdown
extern std::atomic<bool> g_running;

// Полноценная конфигурация (без hardcode)
struct AppConfig {
    std::string base_url;
    std::string agent_uid;
    std::string agent_desc;
    int poll_interval_sec = 60;
    int max_reg_retries = 3;
    int max_backoff_sec = 300;
    std::string task_dir = "./tasks";
    std::string result_dir = "./results";

    bool load(const std::string& path);
};

extern AppConfig g_config;

struct AgentState {
    std::string access_code;
    bool is_registered() const { return !access_code.empty(); }
    void reset() { access_code.clear(); }
};

extern AgentState g_state;

struct Task {
    std::string task_code;
    std::string session_id;
    std::string options;
    std::string status;
};

namespace Logger {
    enum class Level { DEBUG, INFO, WARN, ERR, CRIT };
    void log(Level level, const std::string& session_id, const std::string& msg);
    void debug(const std::string& m, const std::string& sid = "-");
    void info(const std::string& m, const std::string& sid = "-");
    void warn(const std::string& m, const std::string& sid = "-");
    void err (const std::string& m, const std::string& sid = "-");
    void crit(const std::string& m, const std::string& sid = "-");
}

namespace Http {
    struct Response {
        int         status_code = 0;
        std::string body;
    };
    Response post(const std::string& url, const std::string& body_json);
}

namespace Json {
    std::optional<std::string> get(const std::string& json, const std::string& key);
    std::string build(std::initializer_list<std::pair<std::string,std::string>> pairs);
}

bool register_agent();
void polling_loop();
bool send_result(
    const std::string& session_id,
    int result_code,
    const std::string& message,
    const std::vector<std::string>& file_paths
);
