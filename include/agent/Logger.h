#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace agent {

class Logger {
public:
    static void init(const LoggingConfig& config);
    static std::shared_ptr<spdlog::logger> getLogger();
    
    // Convenience methods with structured logging
    static void trace(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
    static void debug(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
    static void info(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
    static void warning(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
    static void error(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "", int error_code = 0);
    static void critical(const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
    
private:
    static std::shared_ptr<spdlog::logger> logger_;
    static LoggingConfig config_;
};

} // namespace agent
