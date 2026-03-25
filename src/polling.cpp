// polling.cpp - @t9tu0
// цикл опроса сервера, получение и выполнение заданий

#include "agent.h"
#include <iostream>
#include <thread>
#include <fstream>

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
            if (!out_task.options.empty()) {
                Logger::info("Options raw: " + out_task.options);
            }
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

// парсит options и извлекает интервал
static int parse_interval_from_options(const std::string& options) {
    if (options.empty()) return -1;

    // пробуем парсить как JSON: {"interval":"15"}
    auto interval_str = Json::get(options, "interval");
    if (interval_str) {
        try {
            return std::stoi(*interval_str);
        } catch (...) {}
    }

    // пробуем парсить как простой текст: {POLL_INTERVAL_SEC = 15}
    size_t pos = options.find("POLL_INTERVAL_SEC");
    if (pos != std::string::npos) {
        size_t eq = options.find("=", pos);
        if (eq != std::string::npos) {
            size_t start = eq + 1;
            while (start < options.size() && options[start] == ' ') start++;
            std::string num;
            while (start < options.size() && (isdigit(options[start]) || options[start] == '-')) {
                num += options[start];
                start++;
            }
            try {
                return std::stoi(num);
            } catch (...) {}
        }
    }

    return -1;
}

// если сервер передал интервал в поле options - применяем его
static void apply_interval_from_options(const std::string& options) {
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
            Logger::info("Конфигурация сохранена");
        }
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

// диспетчер - по task_code выбираем обработчик
using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF", handle_conf },
    { "FILE", handle_file },
    { "TIMEOUT", handle_timeout },
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
