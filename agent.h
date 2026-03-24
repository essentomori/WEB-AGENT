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

// настройки агента, меняй здесь
namespace Config {
    const std::string BASE_URL   = "https://xdev.arkcom.ru:9999/app/webagent1/api";
    const std::string AGENT_UID  = "007";
    const std::string AGENT_DESC = "web-agent";

    // если агент уже зарегистрирован - вставь сюда access_code
    const std::string HARDCODED_ACCESS_CODE = "";

    // интервал опроса по умолчанию в секундах
    // можно переопределить аргументом: ./web_agent 10
    // или сервер может изменить его через поле options в задании
    inline int POLL_INTERVAL_SEC = 5;

    const int MAX_REG_RETRIES = 3;
}

// текущее состояние агента
struct AgentState {
    std::string access_code;
    bool is_registered() const { return !access_code.empty(); }
    void reset() { access_code.clear(); }
};

extern AgentState g_state;

// данные задания которое пришло с сервера
struct Task {
    std::string task_code;
    std::string session_id;
    std::string options;
    std::string status;
};

namespace Logger {
    enum class Level { INFO, WARN, ERR, CRIT };
    void log(Level level, const std::string& msg);
    void info(const std::string& m);
    void warn(const std::string& m);
    void err (const std::string& m);
    void crit(const std::string& m);
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

// функции из других файлов
bool register_agent();
void polling_loop();
bool send_result(
    const std::string& session_id,
    int result_code,
    const std::string& message,
    const std::vector<std::string>& file_paths
);
