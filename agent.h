/**
* ============================================================
 *  agent.h — общий заголовочный файл проекта WebAgent
 *  Подключается во ВСЕ модули проекта
 *
 *  Сборка всего проекта:
 *      g++ -std=c++17 -O2 -o web_agent \
 *          main.cpp polling.cpp result_sender.cpp \
 *          -lcurl
 * ============================================================
 */
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

// ============================================================
//  Конфигурация — меняй здесь
// ============================================================
namespace Config {
    const std::string BASE_URL   = "https://xdev.arkcom.ru:9999/app/webagent1/api";
    const std::string AGENT_UID  = "007";
    const std::string AGENT_DESC = "web-agent";

    // Если агент уже зарегистрирован — вставь access_code сюда:
    const std::string HARDCODED_ACCESS_CODE = "";

    const int POLL_INTERVAL_SEC = 5;
    const int MAX_REG_RETRIES   = 3;
}

// ============================================================
//  Глобальное состояние агента
// ============================================================
struct AgentState {
    std::string access_code;
    bool is_registered() const { return !access_code.empty(); }
    void reset() { access_code.clear(); }
};
extern AgentState g_state;

// ============================================================
//  Структура задания
// ============================================================
struct Task {
    std::string task_code;
    std::string session_id;
    std::string options;
    std::string status;
};

// ============================================================
//  Logger
// ============================================================
namespace Logger {
    enum class Level { INFO, WARN, ERR, CRIT };
    void log(Level level, const std::string& msg);
    void info(const std::string& m);
    void warn(const std::string& m);
    void err (const std::string& m);
    void crit(const std::string& m);
}

// ============================================================
//  Http
// ============================================================
namespace Http {
    struct Response {
        int         status_code = 0;
        std::string body;
    };
    Response post(const std::string& url, const std::string& body_json);
}

// ============================================================
//  Json
// ============================================================
namespace Json {
    std::optional<std::string> get(const std::string& json, const std::string& key);
    std::string build(std::initializer_list<std::pair<std::string,std::string>> pairs);
}

// ============================================================
//  Объявления функций модулей
// ============================================================
bool register_agent();
void polling_loop();
bool send_result(
    const std::string& session_id,
    int result_code,
    const std::string& message,
    const std::vector<std::string>& file_paths
);
