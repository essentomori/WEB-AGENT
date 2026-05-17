#include "agent.h"
#include <iostream>
#include <curl/curl.h>
#include <fstream>
#include <sstream>

AgentState g_state;

namespace Logger {
    void log(Level level, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[20];
#ifdef _WIN32
        struct tm tm_buf;
        localtime_s(&tm_buf, &t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#else
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#endif
        const char* tag = "";
        switch (level) {
            case Level::INFO: tag = "[INFO] "; break;
            case Level::WARN: tag = "[WARN] "; break;
            case Level::ERR:  tag = "[ERR]  "; break;
            case Level::CRIT: tag = "[CRIT] "; break;
        }
        std::cout << buf << " " << tag << msg << "\n" << std::flush;
    }
    void info(const std::string& m) { log(Level::INFO, m); }
    void warn(const std::string& m) { log(Level::WARN, m); }
    void err (const std::string& m) { log(Level::ERR,  m); }
    void crit(const std::string& m) { log(Level::CRIT, m); }
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
        headers = curl_slist_append(headers, "Accept: application/json");

        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST,           1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body_json.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)body_json.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::string e = curl_easy_strerror(res);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw std::runtime_error("curl error: " + e);
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = (int)http_code;

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return resp;
    }
}

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

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return true;
}

// Динамическая загрузка ВСЕХ параметров из файла assets/config.json
static bool load_config_file() {
    std::string cfg = read_file(CONFIG_FILE_PATH);
    if (cfg.empty()) return false;

    // Читаем UID, если он есть в файле
    auto uid_opt = Json::get(cfg, "uid");
    if (uid_opt) {
        Config::AGENT_UID = *uid_opt;
    }

    // Читаем описание, если оно есть
    auto desc_opt = Json::get(cfg, "description");
    if (desc_opt) {
        Config::AGENT_DESC = *desc_opt;
    }

    // Читаем интервал опроса
    auto interval_opt = Json::get(cfg, "poll_interval_sec");
    if (interval_opt) {
        try {
            int iv = std::stoi(*interval_opt);
            if (iv > 0) Config::POLL_INTERVAL_SEC = iv;
        } catch(...) {}
    }

    // Проверяем наличие токена доступа
    auto code_opt = Json::get(cfg, "access_code");
    if (code_opt && !code_opt->empty()) {
        g_state.access_code = *code_opt;
        Logger::info("Конфигурация успешно загружена из " + std::string(CONFIG_FILE_PATH) + ". UID=" + Config::AGENT_UID + ", Интервал=" + std::to_string(Config::POLL_INTERVAL_SEC) + "с");
        return true;
    }

    Logger::info("Конфиг найден, но access_code пуст. Требуется регистрация для UID=" + Config::AGENT_UID);
    return false;
}

static bool save_access_code(const std::string& code) {
    std::string cfg = read_file(CONFIG_FILE_PATH);
    if (cfg.empty()) {
        cfg = "{\"uid\":\"" + Config::AGENT_UID + "\",\"description\":\"" + Config::AGENT_DESC + "\",\"poll_interval_sec\":" + std::to_string(Config::POLL_INTERVAL_SEC) + "}";
    }

    size_t pos = cfg.find("\"access_code\"");
    if (pos != std::string::npos) {
        size_t colon = cfg.find(':', pos);
        if (colon != std::string::npos) {
            size_t start = cfg.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cfg.find('"', start + 1);
                if (end != std::string::npos) {
                    cfg.replace(start + 1, end - start - 1, code);
                    return write_file(CONFIG_FILE_PATH, cfg);
                }
            }
        }
    } else {
        size_t brace = cfg.rfind('}');
        if (brace != std::string::npos) {
            std::string insert = ",\"access_code\":\"" + code + "\"";
            cfg.insert(brace, insert);
            return write_file(CONFIG_FILE_PATH, cfg);
        }
    }
    return false;
}

bool register_agent() {
    Logger::info("Регистрация агента UID=" + Config::AGENT_UID + " ...");

    std::string url  = Config::BASE_URL + "/wa_reg/";
    std::string body = Json::build({
        {"UID",   Config::AGENT_UID},
        {"descr", Config::AGENT_DESC}
    });

    try {
        auto resp = Http::post(url, body);
        Logger::info("Ответ (" + std::to_string(resp.status_code) + "): " + resp.body);

        if (resp.status_code != 200) {
            Logger::err("HTTP ошибка: " + std::to_string(resp.status_code));
            return false;
        }

        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) {
            Logger::err("Не удалось разобрать ответ сервера");
            return false;
        }

        int code = std::stoi(*code_opt);

        if (code == 0) {
            std::string new_code = Json::get(resp.body, "access_code").value_or("");
            if (new_code.empty()) {
                Logger::err("Ответ не содержит access_code");
                return false;
            }
            g_state.access_code = new_code;
            Logger::info("Регистрация успешна. access_code=" + g_state.access_code);
            save_access_code(g_state.access_code);
            return true;
        }
        else if (code == -3) {
            Logger::warn("Агент с UID=" + Config::AGENT_UID + " уже зарегистрирован на сервере (code=-3).");
            g_state.access_code = Config::HARDCODED_ACCESS_CODE;
            save_access_code(g_state.access_code);
            return true;
        } else {
            Logger::err("Неожиданный code_responce=" + *code_opt);
            return false;
        }
    } catch (const std::exception& e) {
        Logger::err(std::string("Ошибка при регистрации: ") + e.what());
        return false;
    }
}

int main(int argc, char* argv[]) {
    Logger::info("=== WebAgent запускается ===");

    // Шаг 1: Сначала строго читаем файл из assets
    bool has_access_code = load_config_file();

    // Шаг 2: Если передан аргумент командной строки, он имеет высший приоритет для интервала
    if (argc > 1) {
        try {
            int iv = std::stoi(argv[1]);
            if (iv > 0) {
                Config::POLL_INTERVAL_SEC = iv;
                Logger::info("Интервал из аргумента: " + std::to_string(iv) + " сек.");
            }
        } catch (...) {}
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!has_access_code) {
        Logger::info("Токен доступа не найден в assets/config.json, отправляем запрос регистрации...");
        register_agent();
    }

    if (g_state.access_code.empty()) {
        Logger::crit("Критическая ошибка: отсутствует access_code. Завершение работы.");
        curl_global_cleanup();
        return 1;
    }

    polling_loop();

    curl_global_cleanup();
    Logger::info("=== WebAgent завершил работу ===");
    return 0;
}
