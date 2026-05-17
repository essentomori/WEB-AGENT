// result_sender.cpp
// Отправка результата задания: multipart/form-data
// result_code — отдельное поле формы (не в JSON), result — JSON со всеми остальными полями

#include "agent.h"
#include <iostream>
#include <fstream>
#include <curl/curl.h>

// Callback для libcurl — складывает ответ в строку
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Формирует JSON-тело поля "result" (result_code сюда НЕ входит — он идёт отдельным полем)
static std::string build_result_json(
    const std::string& session_id,
    const std::string& message,
    int file_count)
{
    return Json::build({
        {"UID",         Config::AGENT_UID},
        {"descr",       Config::AGENT_DESC},
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
    Logger::info("Отправка результата: session_id=" + session_id
                 + " result_code=" + std::to_string(result_code)
                 + " files=" + std::to_string(file_paths.size()));

    std::string url = Config::BASE_URL + "/wa_result/";

    // Считаем только существующие файлы
    int real_file_count = 0;
    for (const auto& p : file_paths) {
        std::ifstream f(p);
        if (f.good()) ++real_file_count;
    }

    // JSON для поля "result" — result_code здесь отсутствует намеренно
    std::string result_json = build_result_json(session_id, message, real_file_count);

    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::err("curl_easy_init() failed");
        return false;
    }

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part;

    // Поле result_code — отдельное поле формы, именно его ждёт сервер
    // Отправляем как строку (сервер принимает и "0", и 0)
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "result_code");
    std::string rc_str = std::to_string(result_code);
    curl_mime_data(part, rc_str.c_str(), CURL_ZERO_TERMINATED);

    // Поле result — JSON со всеми метаданными задания
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "result");
    curl_mime_data(part, result_json.c_str(), CURL_ZERO_TERMINATED);

    // Файловые вложения: file1, file2, ...
    int idx = 1;
    for (const auto& path : file_paths) {
        std::ifstream f(path);
        if (!f.good()) {
            Logger::warn("Файл не найден, пропускаю: " + path);
            continue;
        }
        std::string field = "file" + std::to_string(idx++);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, field.c_str());
        curl_mime_filedata(part, path.c_str());
        Logger::info("Прикрепляю: " + field + " = " + path);
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

    Logger::info("Ответ сервера (" + std::to_string(http_code) + "): " + response_body);

    if (http_code != 200) {
        Logger::err("HTTP ошибка при отправке результата: " + std::to_string(http_code));
        return false;
    }

    // Проверяем ACK от сервера
    auto code_opt = Json::get(response_body, "code_responce");
    if (!code_opt) {
        Logger::err("Не удалось разобрать ACK сервера: " + response_body);
        return false;
    }

    int srv_code = std::stoi(*code_opt);
    if (srv_code == 0) {
        Logger::info("Результат принят сервером. OK.");
        return true;
    }

    auto msg = Json::get(response_body, "msg").value_or("?");
    Logger::err("Сервер вернул ошибку в ACK: " + msg);
    return false;
}
