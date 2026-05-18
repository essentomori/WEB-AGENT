#pragma once

#include <string>
#include <sstream>
#include <optional>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <stdexcept>
#include <functional>
#include <map>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>

// ── Config (все значения — runtime, никаких hardcode) ────────────────────────

namespace Config {
    inline std::string CONFIG_FILE;            // путь к config.json, задаётся при запуске
    inline std::string BASE_URL;               // обязателен — из config.json / env
    inline std::string AGENT_UID;              // обязателен
    inline std::string AGENT_DESC    = "web-agent";
    inline int  POLL_INTERVAL_SEC    = 60;
    inline int  BACKOFF_MAX_SEC      = 300;
    inline int  MAX_REG_RETRIES      = 3;
    inline long REQUEST_TIMEOUT_SEC  = 30;
    inline long CONNECT_TIMEOUT_SEC  = 10;
    inline std::string TASK_DIR      = "tasks";
    inline std::string RESULT_DIR    = "results";
    inline std::string LOG_DIR       = "logs";

    // Загружает config.json, затем AGENT_BASE_URL / AGENT_UID из env.
    // Бросает std::runtime_error если BASE_URL или UID отсутствуют.
    void load(const std::string& path);
    bool save_access_code(const std::string& code);
    bool load_access_code(std::string& out);
}

// ── AgentState ────────────────────────────────────────────────────────────────

struct AgentState {
    std::string           access_code;
    std::atomic<bool>     shutdown_requested{false};
    bool is_registered() const { return !access_code.empty(); }
    void reset()               { access_code.clear(); }
};

extern AgentState g_state;

// ── Task ──────────────────────────────────────────────────────────────────────

struct Task {
    std::string task_code;
    std::string session_id;
    std::string options;
    std::string status;
    // Валидация: task_code и session_id обязаны быть непустыми
    bool is_valid() const { return !task_code.empty() && !session_id.empty(); }
};

// ── Logger (потокобезопасный, структурированный) ──────────────────────────────

namespace Logger {
    enum class Level { DEBUG = 0, INFO, WARNING, ERR, CRIT };

    // Инициализация (вызвать один раз до первого лога)
    void init(Level min_level = Level::INFO,
              bool  to_file   = false,
              const std::string& log_file = "");

    // Ядро: [Timestamp.ms] [LEVEL  ] [T:tid] [task=X] [session=Y] msg
    void log(Level             level,
             const std::string& msg,
             const std::string& task_id    = "",
             const std::string& session_id = "");

    // Удобные обёртки (с дефолтными полями — полная обратная совместимость)
    inline void debug(const std::string& m,
                      const std::string& t="", const std::string& s="")
        { log(Level::DEBUG,   m, t, s); }
    inline void info (const std::string& m,
                      const std::string& t="", const std::string& s="")
        { log(Level::INFO,    m, t, s); }
    inline void warn (const std::string& m,
                      const std::string& t="", const std::string& s="")
        { log(Level::WARNING, m, t, s); }
    inline void err  (const std::string& m,
                      const std::string& t="", const std::string& s="")
        { log(Level::ERR,     m, t, s); }
    inline void crit (const std::string& m,
                      const std::string& t="", const std::string& s="")
        { log(Level::CRIT,    m, t, s); }
}

// ── Http ──────────────────────────────────────────────────────────────────────

namespace Http {
    struct Response {
        int         status_code = 0;
        std::string body;
    };
    Response post(const std::string& url, const std::string& body_json);
}

// ── Json ──────────────────────────────────────────────────────────────────────

namespace Json {
    std::optional<std::string> get(const std::string& json, const std::string& key);
    std::string build(std::initializer_list<std::pair<std::string,std::string>> pairs);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool register_agent();
void polling_loop();
bool send_result(const std::string& session_id,
                 int                result_code,
                 const std::string& message,
                 const std::vector<std::string>& file_paths);
