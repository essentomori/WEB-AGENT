/**
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  РАЗРАБОТЧИК №2: @t9tu0                                     ║
 * ║  ФАЙЛ: polling.cpp                                          ║
 * ║                                                             ║
 * ║  Отвечает за:                                               ║
 * ║    • Цикл опроса сервера каждые N секунд                    ║
 * ║    • Запрос задания  POST /api/wa_task/                     ║
 * ║    • При status=WAIT — ожидание следующего цикла            ║
 * ║    • При status=RUN  — передача задания на выполнение       ║
 * ║    • При code_responce=-2 — повторная регистрация           ║
 * ║    • Диспетчер обработчиков по task_code                    ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include "agent.h"
#include <iostream>
#include <thread>

// ============================================================
//  Запрос задания  POST /api/wa_task/
//
//  Запрос:
//    { "UID":"007", "descr":"web-agent", "access_code":"..." }
//
//  Ответ — задание есть (code_responce=1):
//    { "code_responce":"1", "task_code":"CONF",
//      "options":"", "session_id":"bvLeD2gv-...", "status":"RUN" }
//
//  Ответ — заданий нет (code_responce=0):
//    { "code_responce":"0", "status":"WAIT" }
//
//  Ответ — неверный код доступа (code_responce=-2):
//    { "code_responce":"-2", "msg":"неверный код доступа" }
//
//  Возвращает:
//     1   — задание получено, out_task заполнен
//     0   — заданий нет (WAIT)
//    -2   — неверный код доступа → нужна повторная регистрация
//   -99   — сетевая или иная ошибка
// ============================================================
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
            // Задание есть — заполняем структуру
            out_task.task_code  = Json::get(resp.body, "task_code") .value_or("");
            out_task.session_id = Json::get(resp.body, "session_id").value_or("");
            out_task.options    = Json::get(resp.body, "options")   .value_or("");
            out_task.status     = Json::get(resp.body, "status")    .value_or("");
            Logger::info("Получено задание:"
                         " task_code=" + out_task.task_code +
                         " session_id=" + out_task.session_id +
                         " status=" + out_task.status);
            return 1;

        } else if (code == 0) {
            Logger::info("Заданий нет (WAIT). Следующий опрос через "
                         + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");
            return 0;

        } else if (code == -2) {
            Logger::err("Неверный код доступа (code_responce=-2). "
                        "Запускаем повторную регистрацию.");
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

// ============================================================
//  Обработчики заданий по task_code
// ============================================================

// Задание конфигурации (task_code = "CONF")
static void handle_conf(const Task& task) {
    Logger::info("[CONF] Начинаю обработку, session_id=" + task.session_id);

    // TODO: здесь бизнес-логика задания CONF
    // Например: прочитать конфиг, применить настройки...

    Logger::info("[CONF] Задание выполнено. Отправляем результат...");

    // Вызов модуля @miroslav_pug (result_sender.cpp)
    // Передаём: session_id, код результата (0=ок), сообщение, список файлов
    bool ok = send_result(task.session_id, 0, "конфигурация применена", {});
    if (ok) {
        Logger::info("[CONF] Результат успешно отправлен.");
    } else {
        Logger::err("[CONF] Не удалось отправить результат.");
    }
}

// Диспетчер: task_code → обработчик
using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "CONF", handle_conf },
    // Добавляй новые task_code здесь:
    // { "SCAN", handle_scan },
    // { "UPDATE", handle_update },
};

static void execute_task(const Task& task) {
    Logger::info("▶ Выполняю task_code=" + task.task_code
                 + " session_id=" + task.session_id);

    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неизвестный task_code='" + task.task_code + "'. Пропускаю.");
        // Сообщаем серверу что задание не выполнено
        send_result(task.session_id, -1, "неизвестный task_code: " + task.task_code, {});
    }
}

// ============================================================
//  Повторная регистрация с экспоненциальной задержкой
// ============================================================
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
        Logger::info("Ждём " + std::to_string(backoff) + " сек. перед следующей попыткой...");
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }

    Logger::crit("Повторная регистрация не удалась после "
                 + std::to_string(Config::MAX_REG_RETRIES) + " попыток.");
    return false;
}

// ============================================================
//  Главный цикл опроса — вызывается из main() (@essentomori)
// ============================================================
void polling_loop() {
    Logger::info("Цикл опроса запущен. Интервал: "
                 + std::to_string(Config::POLL_INTERVAL_SEC) + " сек.");

    while (true) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            // Задание получено — выполняем
            execute_task(task);

        } else if (result == -2) {
            // Неверный код доступа — повторная регистрация
            if (!try_reregister()) {
                Logger::crit("Не удалось восстановить доступ. Завершение работы.");
                return;
            }
            // Сразу опрашиваем после успешной регистрации
            continue;

        } else {
            // result == 0 (WAIT) или -99 (ошибка сети) — просто ждём
        }

        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
