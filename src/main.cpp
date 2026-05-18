#include "agent.h"
#include <iostream>
#include <csignal>
#include <curl/curl.h>

// Инициализация глобального состояния
AgentState g_state;

// Обработчик сигналов завершения (Graceful Shutdown)
void signal_handler(int signum) {
    Logger::info("Получен сигнал завершения (" + std::to_string(signum) + "). Завершаем работу...");
    g_state.shutdown_requested.store(true);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  РЕАЛИЗАЦИЯ HTTP И JSON (Которые были удалены из старого main.cpp)
// ═══════════════════════════════════════════════════════════════════════════════

namespace Json {
    // Собирает простой JSON из пар ключ-значение
    std::string build(std::initializer_list<std::pair<std::string, std::string>> pairs) {
        std::string json = "{";
        bool first = true;
        for (const auto& p : pairs) {
            if (!first) json += ",";
            json += "\"" + p.first + "\":\"" + p.second + "\"";
            first = false;
        }
        json += "}";
        return json;
    }

    // Извлекает значение по ключу из строки JSON
    std::optional<std::string> get(const std::string& json, const std::string& key) {
        std::string search_key = "\"" + key + "\"";
        size_t pos = json.find(search_key);
        if (pos == std::string::npos) return std::nullopt;

        pos = json.find(":", pos + search_key.length());
        if (pos == std::string::npos) return std::nullopt;

        size_t start = json.find_first_not_of(" \t\n\r", pos + 1);
        if (start == std::string::npos) return std::nullopt;

        if (json[start] == '"') {
            size_t end = json.find('"', start + 1);
            if (end == std::string::npos) return std::nullopt;
            return json.substr(start + 1, end - start - 1);
        } else {
            size_t end = json.find_first_of(",}", start);
            if (end == std::string::npos) end = json.length();
            std::string val = json.substr(start, end - start);
            val.erase(val.find_last_not_of(" \t\n\r") + 1);
            return val;
        }
    }
}

namespace Http {
    // Вспомогательный callback для записи ответа сервера
    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
        static_cast<std::string*>(ud)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    // Классический POST запрос через чистый libcurl (для регистрации и опроса)
    Response post(const std::string& url, const std::string& body_json) {
        Response response;
        CURL* curl = curl_easy_init();
        if (!curl) return response;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            response.status_code = static_cast<int>(code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return response;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ТОЧКА ВХОДА MAIN
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    Logger::init(Logger::Level::INFO, false, "");
    Logger::info("=== WebAgent запускается ===");

    std::string config_path = "assets/config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    try {
        Config::load(config_path);
        Logger::info("Конфигурация успешно загружена: " + config_path);
    } catch (const std::exception& e) {
        Logger::crit("Ошибка загрузки конфигурации: " + std::string(e.what()));
        return 1;
    }

    std::string saved_code;
    if (Config::load_access_code(saved_code) && !saved_code.empty()) {
        g_state.access_code = saved_code;
        Logger::info("Токен доступа успешно загружен из конфига.");
    } else {
        Logger::info("Токен не найден. Требуется регистрация на сервере...");
        if (!register_agent()) {
            Logger::crit("Критическая ошибка: не удалось зарегистрировать агента. Завершение.");
            return 1;
        }
    }

    // Запуск многопоточного бесконечного цикла опроса
    polling_loop();

    Logger::info("=== WebAgent завершил работу ===");
    return 0;
}
