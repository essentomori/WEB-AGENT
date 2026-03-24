#include "agent.h"
#include <fstream>
#include <curl/curl.h>

static bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string build_result_json(
    const std::string& session_id,
    const std::string& message,
    int file_count)
{
    return Json::build({
        {"UID",         Config::AGENT_UID},
        {"access_code", g_state.access_code},
        {"message",     message},
        {"files",       std::to_string(file_count)},
        {"session_id",  session_id}
    });
}

bool send_result(
    const std::string& session_id,
    int result_code,
    const std::string& message,
    const std::vector<std::string>& file_paths)
{
    Logger::info("Отправка результата: session_id=" + session_id +
                 " code=" + std::to_string(result_code) +
                 " files=" + std::to_string(file_paths.size()));

    std::string url = Config::BASE_URL + "/wa_result/";
    std::string result_json = build_result_json(session_id, message, (int)file_paths.size());

    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::err("curl_easy_init() failed");
        return false;
    }

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part;

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "result_code");
    curl_mime_data(part, std::to_string(result_code).c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "result");
    curl_mime_data(part, result_json.c_str(), CURL_ZERO_TERMINATED);

    int attached = 0;
    for (size_t i = 0; i < file_paths.size(); ++i) {
        if (!file_exists(file_paths[i])) {
            Logger::warn("Файл не найден: " + file_paths[i]);
            continue;
        }
        std::string field_name = "file" + std::to_string(i + 1);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, field_name.c_str());
        curl_mime_filedata(part, file_paths[i].c_str());
        attached++;
    }

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST,       mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        Logger::err("curl ошибка: " + std::string(curl_easy_strerror(res)));
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    Logger::info("Ответ сервера: " + response_body);

    if (http_code != 200) {
        Logger::err("HTTP ошибка: " + std::to_string(http_code));
        return false;
    }

    auto code_opt = Json::get(response_body, "code_responce");
    if (!code_opt) {
        Logger::err("Не удалось разобрать ответ");
        return false;
    }

    if (std::stoi(*code_opt) == 0) {
        Logger::info("Результат принят");
        return true;
    } else {
        Logger::err("Сервер вернул ошибку: " + response_body);
        return false;
    }
}
