// polling.cpp - @t9tu0
// цикл опроса сервера, получение и выполнение заданий

#include "agent.h"
#include <iostream>
#include <thread>

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
                         + " session_id=" + out_task.session_id
                         + " status=" + out_task.status);
            return 1;
        } else if (code == 0) {
            Logger::info("Заданий нет (WAIT). Следующий опрос через "
                         + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");
            return 0;
        } else if (code == -2) {
            Logger::err("Неверный код доступа. Запускаем повторную регистрацию.");
            return -2;
        } else {
            Logger::warn("Неожиданный ответ сервера: " + resp.body);
            return -99;
        }
    } catch (const std::exception& e) {
        Logger::err(std::string("Сетевая ошибка при запросе задания: ") + e.what());
        return -99;
    }
}

// если сервер передал интервал в поле options - применяем его
// сервер может прислать: {"interval":"10"}
static void apply_interval_from_options(const std::string& options) {
    if (options.empty()) return;

    auto interval_str = Json::get(options, "interval");
    if (!interval_str) return;

    try {
        int new_interval = std::stoi(*interval_str);
        if (new_interval > 0 && new_interval != Config::POLL_INTERVAL_SEC) {
            Config::POLL_INTERVAL_SEC = new_interval;
            Logger::info("Сервер изменил интервал опроса: "
                         + std::to_string(new_interval) + " сек.");
        }
    } catch (...) {
        Logger::warn("Не удалось прочитать интервал из options: " + options);
    }
}

// обработка задания типа CONF
static void handle_conf(const Task& task) {
    Logger::info("[CONF] Начинаю обработку, session_id=" + task.session_id);

    // тут должна быть бизнес-логика задания
    // например: прочитать конфиг, применить настройки, собрать файлы

    Logger::info("[CONF] Задание выполнено. Отправляем результат...");

    bool ok = send_result(task.session_id, 0, "конфигурация применена", {});
    if (ok) {
        Logger::info("[CONF] Результат принят сервером.");
    } else {
        Logger::err("[CONF] Не удалось отправить результат.");
    }
}

// диспетчер - по task_code выбираем обработчик
using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF", handle_conf },
    // { "SCAN",   handle_scan   },
    // { "UPDATE", handle_update },
};

static void execute_task(const Task& task) {
    Logger::info("Выполняю task_code=" + task.task_code
                 + " session_id=" + task.session_id);

    // проверяем не хочет ли сервер изменить интервал опроса
    apply_interval_from_options(task.options);

    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неизвестный task_code='" + task.task_code + "'. Пропускаю.");
        send_result(task.session_id, -1, "неизвестный task_code: " + task.task_code, {});
    }
}

// повторная регистрация если сервер вернул -2
static bool try_reregister() {
    g_state.reset();

    for (int attempt = 1; attempt <= Config::MAX_REG_RETRIES; ++attempt) {
        Logger::info("Повторная регистрация: попытка "
                     + std::to_string(attempt) + "/"
                     + std::to_string(Config::MAX_REG_RETRIES));

        if (register_agent()) {
            Logger::info("Повторная регистрация успешна.");
            return true;
        }

        int backoff = attempt * 5;
        Logger::info("Ждём " + std::to_string(backoff) + " сек...");
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }

    Logger::crit("Повторная регистрация не удалась после всех попыток.");
    return false;
}

// главный цикл - вызывается из main()
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
                Logger::crit("Не удалось восстановить доступ. Завершение.");
                return;
            }
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
