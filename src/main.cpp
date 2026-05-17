#include "agent.h"
#include <iostream>
#include <curl/curl.h>
#include <fstream>
#include <csignal>

AppConfig g_config;
AgentState g_state;
std::atomic<bool> g_running{true};
std::mutex g_log_mutex; // Для потокобезопасного логгирования

void signal_handler(int signum) {
    g_running = false;
    std::cout << "\n[CRIT] Получен сигнал " << signum << ". Завершение работы...\n";
}

// Загрузка конфигурации
bool AppConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string json = ss.str();

    base_url = Json::get(json, "base_url").value_or("https://localhost/api");
    agent_uid = Json::get(json, "uid").value_or("unknown");
    agent_desc = Json::get(json, "description").value_or("web-agent");

    auto interval = Json::get(json, "poll_interval_sec");
    if (interval) poll_interval_sec = std::stoi(*interval);

    return true;
}

namespace Logger {
    void log(Level level, const std::string& session_id, const std::string& msg) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[20];
#ifdef _WIN32
        struct tm tm_buf; localtime_s(&tm_buf, &t); std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#else
        struct tm tm_buf; localtime_r(&t, &tm_buf); std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#endif
        const char* tag = "";
        switch (level) {
            case Level::DEBUG: tag = "[DEBUG]"; break;
            case Level::INFO:  tag = "[INFO] "; break;
            case Level::WARN:  tag = "[WARN] "; break;
            case Level::ERR:   tag = "[ERR]  "; break;
            case Level::CRIT:  tag = "[CRIT] "; break;
        }
        std::cout << buf << " " << tag << " [" << session_id << "] " << msg << "\n" << std::flush;
    }
    void debug(const std::string& m, const std::string& sid) { log(Level::DEBUG, sid, m); }
    void info(const std::string& m, const std::string& sid)  { log(Level::INFO, sid, m); }
    void warn(const std::string& m, const std::string& sid)  { log(Level::WARN, sid, m); }
    void err (const std::string& m, const std::string& sid)  { log(Level::ERR, sid, m); }
    void crit(const std::string& m, const std::string& sid)  { log(Level::CRIT, sid, m); }
}

namespace Http {
    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
        static_cast<std::string*>(ud)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    Response post(const std::string& url, const std::string& body_json) {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init() failed");

        Response resp;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw std::runtime_error(curl_easy_strerror(res));
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = (int)http_code;

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return resp;
    }
}

// (Json::get и Json::build остаются без изменений)
namespace Json {
    std::optional<std::string> get(const std::string& json, const std::string& key) {
        std::string sk = "\"" + key + "\"";
        size_t pos = json.find(sk);
        if (pos == std::string::npos) return std::nullopt;
        pos += sk.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size()) return std::nullopt;
        if (json[pos] == '"') {
            ++pos;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return std::nullopt;
            return json.substr(pos, end - pos);
        } else {
            size_t end = pos;
            while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
            std::string val = json.substr(pos, end - pos);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            return val;
        }
    }

    std::string build(std::initializer_list<std::pair<std::string,std::string>> pairs) {
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        for (auto& [k, v] : pairs) {
            if (!first) ss << ",";
            first = false;
            ss << "\"" << k << "\":\"" << v << "\"";
        }
        ss << "}";
        return ss.str();
    }
}

bool register_agent() {
    Logger::info("Регистрация агента UID=" + g_config.agent_uid + " ...");
    std::string url = g_config.base_url + "/wa_reg/";
    std::string body = Json::build({{"UID", g_config.agent_uid}, {"descr", g_config.agent_desc}});

    try {
        auto resp = Http::post(url, body);
        if (resp.status_code != 200) return false;

        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) return false;

        int code = std::stoi(*code_opt);
        if (code == 0) {
            g_state.access_code = Json::get(resp.body, "access_code").value_or("");
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!g_config.load(CONFIG_FILE_PATH)) {
        std::cerr << "Критическая ошибка: Не удалось загрузить " << CONFIG_FILE_PATH << "\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Загрузка сохраненного access_code из файла состояния или регистрация
    // (Для сокращения листинга предполагается, что access_code запрашивается, если пуст)
    while(g_running && !g_state.is_registered()) {
        if (register_agent()) break;
        Logger::warn("Регистрация не удалась. Повтор через 5 секунд...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    if (g_running) polling_loop();

    curl_global_cleanup();
    Logger::info("=== WebAgent корректно завершил работу ===");
    return 0;
}
