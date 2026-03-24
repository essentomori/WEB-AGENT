#include "agent.h"
#include <thread>
#include <fstream>

static int request_task(Task& out_task) {
    std::string url = Config::BASE_URL + "/wa_task/";
    std::string body = Json::build({
        {"UID", Config::AGENT_UID},
        {"descr", Config::AGENT_DESC},
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
            out_task.task_code = Json::get(resp.body, "task_code").value_or("");
            out_task.session_id = Json::get(resp.body, "session_id").value_or("");
            out_task.options = Json::get(resp.body, "options").value_or("");
            out_task.status = Json::get(resp.body, "status").value_or("");
            Logger::info("Получено задание: " + out_task.task_code + " session=" + out_task.session_id);
            return 1;
        } else if (code == 0) {
            Logger::info("Заданий нет");
            return 0;
        } else if (code == -2) {
            Logger::err("Неверный код доступа");
            return -2;
        } else {
            Logger::warn("Неожиданный ответ: " + resp.body);
            return -99;
        }
    } catch (const std::exception& e) {
        Logger::err("Ошибка: " + std::string(e.what()));
        return -99;
    }
}

static void handle_conf(const Task& task) {
    Logger::info("[CONF] Выполнение, session=" + task.session_id);

    if (!task.options.empty()) {
        size_t pos = task.options.find("POLL_INTERVAL_SEC");
        if (pos != std::string::npos) {
            size_t eq = task.options.find("=", pos);
            if (eq != std::string::npos) {
                try {
                    int new_interval = std::stoi(task.options.substr(eq + 1));
                    if (new_interval > 0) {
                        Config::POLL_INTERVAL_SEC = new_interval;
                        Logger::info("[CONF] Интервал опроса изменён на " + std::to_string(new_interval) + " сек");

                        std::ofstream file("config.json");
                        if (file.is_open()) {
                            file << "{\n";
                            file << "    \"uid\": \"" << Config::AGENT_UID << "\",\n";
                            file << "    \"description\": \"" << Config::AGENT_DESC << "\",\n";
                            file << "    \"poll_interval_sec\": " << new_interval << "\n";
                            file << "}\n";
                            file.close();
                            Logger::info("[CONF] Конфигурация сохранена в config.json");
                        }
                    }
                } catch (...) {
                    Logger::warn("[CONF] Не удалось распарсить интервал");
                }
            }
        }
    }

    send_result(task.session_id, 0, "конфигурация применена", {});
}

static const std::map<std::string, std::function<void(const Task&)>> HANDLERS = {
    {"CONF", handle_conf},
};

static void execute_task(const Task& task) {
    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        it->second(task);
    } else {
        Logger::warn("Неизвестный task_code: " + task.task_code);
        send_result(task.session_id, -1, "неизвестный task_code", {});
    }
}

static bool try_reregister() {
    g_state.reset();
    for (int attempt = 1; attempt <= Config::MAX_REG_RETRIES; ++attempt) {
        Logger::info("Повторная регистрация, попытка " + std::to_string(attempt));
        if (register_agent()) return true;
        std::this_thread::sleep_for(std::chrono::seconds(attempt * 5));
    }
    Logger::crit("Повторная регистрация не удалась");
    return false;
}

void polling_loop() {
    Logger::info("Цикл опроса запущен, интервал " + std::to_string(Config::POLL_INTERVAL_SEC) + " сек");

    while (true) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            execute_task(task);
        } else if (result == -2) {
            if (!try_reregister()) return;
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(Config::POLL_INTERVAL_SEC));
    }
}
