// main.cpp - @essentomori
// точка входа, регистрация, утилиты Logger/Http/Json

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
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
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

// ---------- Функции для работы с config.json ----------
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

// Загружает access_code из config.json (если есть)
static bool load_access_code() {
    std::string cfg = read_file(CONFIG_FILE_PATH);
    if (cfg.empty()) return false;

    size_t pos = cfg.find("\"access_code\"");
    if (pos == std::string::npos) return false;
    size_t colon = cfg.find(':', pos);
    if (colon == std::string::npos) return false;
    size_t start = cfg.find('"', colon);
    if (start == std::string::npos) return false;
    size_t end = cfg.find('"', start + 1);
    if (end == std::string::npos) return false;

    std::string code = cfg.substr(start + 1, end - start - 1);
    if (code.empty()) return false;

    g_state.access_code = code;
    Logger::info("Загружен access_code из config.json: " + code);
    return true;
}

// Сохраняет access_code в config.json
static bool save_access_code(const std::string& code) {
    std::string cfg = read_file(CONFIG_FILE_PATH);
    if (cfg.empty()) {
        // Создаём базовый конфиг
        cfg = "{\"uid\":\"" + Config::AGENT_UID + "\","
              "\"description\":\"" + Config::AGENT_DESC + "\","
              "\"poll_interval_sec\":" + std::to_string(Config::POLL_INTERVAL_SEC) + "}";
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

// Регистрация агента на сервере (получение access_code)
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

        auto code_opt = Json::get(resp.body, "code_responce");  // исправлено на code_responce
        if (!code_opt) {
            Logger::err("Не удалось разобрать ответ");
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
            if (!save_access_code(g_state.access_code)) {
                Logger::err("Не удалось сохранить access_code в config.json");
                return false;
            }
            return true;
        } else if (code == -3) {
            // Агент уже зарегистрирован – пытаемся загрузить код из файла
            Logger::warn("Агент уже зарегистрирован (code=-3)");
            if (load_access_code()) {
                Logger::info("Загружен сохранённый access_code");
                return true;
            }
            Logger::err("Нет сохранённого access_code, регистрация невозможна");
            return false;
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

    // можно задать интервал аргументом: ./WebAgent 30
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

    // Пытаемся загрузить access_code из config.json
    if (!load_access_code()) {
        Logger::info("Сохранённый access_code не найден, выполняем регистрацию");
        if (!register_agent()) {
            // Если регистрация не удалась, используем запасной код (из старых логов)
            Logger::warn("Регистрация не удалась, используем запасной access_code");
            g_state.access_code = "e87ccd-3146-0dcc-2aeb-796c4724";
            // Сохраняем его, чтобы в следующий раз не спрашивать
            if (!save_access_code(g_state.access_code)) {
                Logger::err("Не удалось сохранить запасной access_code");
            } else {
                Logger::info("Запасной access_code сохранён в config.json");
            }
        }
    } else {
        Logger::info("Используем access_code из config.json");
    }

    // Если код всё ещё пуст – завершаем
    if (g_state.access_code.empty()) {
        Logger::crit("Нет access_code. Завершение.");
        curl_global_cleanup();
        return 1;
    }

    polling_loop();

    curl_global_cleanup();
    Logger::info("=== WebAgent завершил работу ===");
    return 0;
}
