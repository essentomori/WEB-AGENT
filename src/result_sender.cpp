#include "agent.h"
#include <iostream>
#include <fstream>
#include <curl/curl.h>

static bool file_exists(const std::string& path) {
    std::ifstream f(path); return f.good();
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb); return size * nmemb;
}

bool send_result(
    const std::string& session_id,
    int result_code,
    const std::string& message,
    const std::vector<std::string>& file_paths)
{
    std::string url = g_config.base_url + "/wa_result/";
    int real_file_count = 0;
    for (auto& p : file_paths) if (file_exists(p)) real_file_count++;

    std::string result_json = Json::build({
        {"UID", g_config.agent_uid},
        {"access_code", g_state.access_code},
        {"message", message},
        {"files", std::to_string(real_file_count)},
        {"session_id", session_id}
    });

    // Retry Logic (попытки отправить результат)
    for (int attempt = 1; attempt <= 3; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part;

        part = curl_mime_addpart(mime); curl_mime_name(part, "result_code");
        std::string rc_str = std::to_string(result_code); curl_mime_data(part, rc_str.c_str(), CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime); curl_mime_name(part, "result");
        curl_mime_data(part, result_json.c_str(), CURL_ZERO_TERMINATED);

        int idx = 1;
        for (auto& path : file_paths) {
            if (file_exists(path)) {
                part = curl_mime_addpart(mime); curl_mime_name(part, ("file" + std::to_string(idx++)).c_str());
                curl_mime_filedata(part, path.c_str());
            }
        }

        std::string response_body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_mime_free(mime); curl_easy_cleanup(curl);

        if (res == CURLE_OK && http_code == 200) {
            Logger::info("Результат успешно отправлен.", session_id);
            return true;
        }

        Logger::warn("Ошибка отправки (попытка " + std::to_string(attempt) + "/3)", session_id);
        std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
    }

    Logger::err("Не удалось отправить результат после 3 попыток.", session_id);
    return false;
}
