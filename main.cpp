/**
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  РАЗРАБОТЧИК №1: @essentomori  (Team Lead)                  ║
 * ║  ФАЙЛ: main.cpp                                             ║
 * ║                                                             ║
 * ║  Отвечает за:                                               ║
 * ║    • Общие утилиты: Logger, Http (libcurl), Json            ║
 * ║    • Регистрацию агента  POST /api/wa_reg/                  ║
 * ║    • Инициализацию и точку входа main()                     ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 *  Сборка всего проекта:
 *      g++ -std=c++17 -O2 -o web_agent \
 *          main.cpp polling.cpp result_sender.cpp -lcurl
 *
 *  Установка зависимости (один раз):
 *      sudo apt install libcurl4-openssl-dev
 */

#include "agent.h"
#include <iostream>
#include <curl/curl.h>

// ============================================================
//  Глобальное состояние агента
// ============================================================
AgentState g_state;

// ============================================================
//  Logger — реализация
// ============================================================
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

// ============================================================
//  Http — POST через libcurl (поддерживает HTTPS)
// ============================================================
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
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // для самоподписанного серт.
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

// ============================================================
//  Json — минимальный парсер/сборщик
// ============================================================
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

// ============================================================
//  Регистрация агента  POST /api/wa_reg/
//
//  Запрос:
//    { "UID":"007", "descr":"web-agent" }
//
//  Ответ успех (code_responce=0):
//    { "code_responce":"0", "msg":"Регистрация прошла успешно",
//      "access_code":"594807-1ddb-36af-9616-d8ed2b9d" }
//
//  Ответ агент уже есть (code_responce=-3):
//    { "code_responce":"-3", "msg":"Такой агент уже зарегисирирован" }
// ============================================================
bool register_agent() {
    Logger::info("Регистрация агента UID=" + Config::AGENT_UID + " ...");

    std::string url  = Config::BASE_URL + "/wa_reg/";
    std::string body = Json::build({
        {"UID",   Config::AGENT_UID},
        {"descr", Config::AGENT_DESC}
    });

    try {
        auto resp = Http::post(url, body);
        Logger::info("Ответ сервера (" + std::to_string(resp.status_code) + "): " + resp.body);

        if (resp.status_code != 200) {
            Logger::err("HTTP ошибка при регистрации: " + std::to_string(resp.status_code));
            return false;
        }

        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) {
            Logger::err("Не удалось разобрать ответ: " + resp.body);
            return false;
        }

        int code = std::stoi(*code_opt);

        if (code == 0) {
            // Успешная регистрация — сохраняем access_code
            g_state.access_code = Json::get(resp.body, "access_code").value_or("");
            Logger::info("Регистрация успешна. access_code=" + g_state.access_code);
            return true;

        } else if (code == -3) {
            // Агент уже зарегистрирован — используем хардкод
            Logger::warn("Агент уже зарегистрирован на сервере.");
            if (!Config::HARDCODED_ACCESS_CODE.empty()) {
                g_state.access_code = Config::HARDCODED_ACCESS_CODE;
                Logger::info("Используем hardcoded access_code=" + g_state.access_code);
                return true;
            }
            Logger::err("HARDCODED_ACCESS_CODE не задан. Установи его в agent.h");
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

// ============================================================
//  Точка входа
// ============================================================
int main() {
    Logger::info("=== WebAgent запускается ===");
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Если access_code задан вручную — пропускаем регистрацию
    if (!Config::HARDCODED_ACCESS_CODE.empty()) {
        g_state.access_code = Config::HARDCODED_ACCESS_CODE;
        Logger::info("Используем hardcoded access_code=" + g_state.access_code);
    } else {
        if (!register_agent()) {
            Logger::crit("Регистрация не удалась. Завершение работы.");
            curl_global_cleanup();
            return 1;
        }
    }

    // Передаём управление циклу опроса (@t9tu0 — polling.cpp)
    polling_loop();

    curl_global_cleanup();
    Logger::info("=== WebAgent завершил работу ===");
    return 0;
}
