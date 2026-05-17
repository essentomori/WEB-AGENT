#include "agent.h"
#include <iostream>
#include <thread>
#include <fstream>
#include <sstream>

// (Вспомогательные функции clean_options, extract_json_object, run_command остаются без изменений)
static std::string run_command(const std::string& cmd) {
    std::array<char, 512> buf; std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "popen failed";
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) result += buf.data();
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

static int request_task(Task& out_task) {
    std::string url  = g_config.base_url + "/wa_task/";
    std::string body = Json::build({
        {"UID",         g_config.agent_uid},
        {"descr",       g_config.agent_desc},
        {"access_code", g_state.access_code}
    });

    try {
        auto resp = Http::post(url, body);
        if (resp.status_code != 200) return -99; // Network/Server error

        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) return -99;

        int code = std::stoi(*code_opt);
        if (code == 1) {
            out_task.task_code  = Json::get(resp.body, "task_code").value_or("");
            out_task.session_id = Json::get(resp.body, "session_id").value_or("");
            return 1;
        } else if (code == 0) {
            return 0; // WAIT
        } else if (code == -2) {
            return -2; // Auth error
        }
        return -99;
    } catch (...) {
        return -99;
    }
}

// Пример обработчика
static void handle_task(const Task& task) {
    Logger::info("Начинаю TASK", task.session_id);
    std::string output = run_command("uname -a"); // Упрощенный парсинг команды для примера
    send_result(task.session_id, 0, output, {});
}

// Диспетчер
using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    { "TASK", handle_task }
    // Остальные хендлеры (CONF, FILE, TIMEOUT) вызываются аналогично
};

static void execute_task(Task task) {
    auto it = HANDLERS.find(task.task_code);
    if (it != HANDLERS.end()) {
        try {
            it->second(task);
        } catch (const std::exception& e) {
            Logger::err(std::string("Ошибка выполнения: ") + e.what(), task.session_id);
            send_result(task.session_id, -1, e.what(), {});
        }
    } else {
        send_result(task.session_id, -1, "Unknown task_code", {});
    }
}

// Главный цикл с Adaptive Backoff и проверкой g_running
void polling_loop() {
    Logger::info("Цикл опроса запущен.");
    int current_interval = g_config.poll_interval_sec;

    while (g_running) {
        Task task;
        int result = request_task(task);

        if (result == 1) {
            current_interval = g_config.poll_interval_sec; // Сброс backoff
            // Асинхронное выполнение задачи (многозадачность)
            std::thread(execute_task, task).detach();
        }
        else if (result == -2) {
            g_state.reset();
            register_agent();
        }
        else if (result == -99) {
            // Adaptive backoff при ошибке сети
            current_interval = std::min(current_interval * 2, g_config.max_backoff_sec);
            Logger::warn("Ошибка сети. Увеличиваем интервал до " + std::to_string(current_interval) + "с.");
        }
        else {
            current_interval = g_config.poll_interval_sec; // WAIT штатный
        }

        // Прерываемый sleep для быстрого Graceful Shutdown
        for(int i = 0; i < current_interval && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
