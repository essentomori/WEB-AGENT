#include "agent/Config.h"
#include "agent/Logger.h"
#include "agent/HttpClient.h"
#include "agent/TaskDispatcher.h"
#include "agent/ResultSender.h"
#include "agent/StateMachine.h"
#include "agent/models/Task.h"

#include <iostream>
#include <signal.h>
#include <atomic>

using namespace agent;

static std::atomic<bool> running{true};
static std::unique_ptr<TaskDispatcher> dispatcher;

void signalHandler(int signal) {
    Logger::info("Received signal " + std::to_string(signal) + ", shutting down...");
    running = false;
    if (dispatcher) {
        dispatcher->stop();
    }
}

class Agent {
public:
    Agent() : http_(Config::getInstance().getServer()) {}
    
    bool initialize() {
        // Setup signal handlers
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
        
        // Load configuration
        auto& config = Config::getInstance();
        
        if (!config.loadFromFile("config/agent.conf")) {
            Logger::warning("Config file not found, using environment variables");
            if (!config.loadFromEnv()) {
                Logger::error("No valid configuration found");
                return false;
            }
        }
        
        // Initialize logger
        Logger::init(config.getLogging());
        
        // Create directories
        createDirectories();
        
        // Initialize task dispatcher
        dispatcher = std::make_unique<TaskDispatcher>(4);
        
        // Register task handlers
        registerHandlers();
        
        state_machine_.setStateCallback([](AgentState state, const std::string& info) {
            Logger::info("State transition: " + std::to_string(static_cast<int>(state)) + " - " + info);
        });
        
        state_machine_.processEvent(StateEvent::INIT_OK);
        
        return true;
    }
    
    void run() {
        Logger::info("Agent starting up", "", "");
        
        while (running) {
            state_machine_.processEvent(StateEvent::RETRY);
            auto state = state_machine_.getCurrentState();
            
            switch (state) {
                case AgentState::REGISTERING:
                    performRegistration();
                    break;
                    
                case AgentState::POLLING:
                    pollForTasks();
                    break;
                    
                case AgentState::ERROR:
                    handleError();
                    break;
                    
                case AgentState::SHUTTING_DOWN:
                    running = false;
                    break;
                    
                default:
                    break;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        cleanup();
    }
    
private:
    void createDirectories() {
        const auto& exec = Config::getInstance().getExecution();
        std::error_code ec;
        std::filesystem::create_directories(exec.task_dir, ec);
        std::filesystem::create_directories(exec.result_dir, ec);
        std::filesystem::create_directories(exec.temp_dir, ec);
        std::filesystem::create_directories(exec.log_dir, ec);
    }
    
    void registerHandlers() {
        auto& registry = TaskHandlerRegistry::getInstance();
        registry.registerHandler(std::make_unique<ConfigTaskHandler>());
        registry.registerHandler(std::make_unique<FileTaskHandler>());
        registry.registerHandler(std::make_unique<CommandTaskHandler>());
        registry.registerHandler(std::make_unique<TimeoutTaskHandler>());
    }
    
    void performRegistration() {
        Logger::info("Registering agent with UID: " + Config::getInstance().getIdentity().uid);
        
        std::string body = R"({
            "UID": ")" + Config::getInstance().getIdentity().uid + R"(",
            "descr": ")" + Config::getInstance().getIdentity().description + R"("
        })";
        
        auto response = http_.post(Config::getInstance().getServer().registration_endpoint, body);
        
        if (response.success()) {
            try {
                auto json = nlohmann::json::parse(response.body);
                if (json.contains("code_responce") && json["code_responce"] == 0) {
                    if (json.contains("access_code")) {
                        std::string code = json["access_code"];
                        Config::getInstance().setAccessCode(code);
                        Logger::info("Registration successful, access_code: " + code);
                        state_machine_.processEvent(StateEvent::REGISTER_OK);
                    }
                } else if (json.contains("code_responce") && json["code_responce"] == -3) {
                    Logger::info("Agent already registered");
                    state_machine_.processEvent(StateEvent::REGISTER_OK);
                }
            } catch (const std::exception& e) {
                Logger::error("Failed to parse registration response: " + std::string(e.what()));
                state_machine_.processEvent(StateEvent::REGISTER_FAIL);
            }
        } else {
            Logger::error("Registration failed: " + response.error_message);
            state_machine_.processEvent(StateEvent::REGISTER_FAIL);
        }
    }
    
    void pollForTasks() {
        auto& config = Config::getInstance();
        auto& polling = config.getPolling();
        
        std::string body = R"({
            "UID": ")" + config.getIdentity().uid + R"(",
            "access_code": ")" + config.getIdentity().access_code.value_or("") + R"("
        })";
        
        auto response = http_.post(config.getServer().task_endpoint, body);
        
        if (!response.success()) {
            Logger::warning("Polling failed: " + response.error_message);
            applyBackoff();
            return;
        }
        
        resetBackoff();
        
        try {
            auto json = nlohmann::json::parse(response.body);
            int code = json.value("code_responce", -1);
            
            if (code == 1) {
                // Task received
                Task task;
                task.task_code = json.value("task_code", "");
                task.session_id = json.value("session_id", "");
                task.options = json.value("options", "");
                task.status = json.value("status", "");
                
                state_machine_.processEvent(StateEvent::TASK_RECEIVED);
                
                // Submit to dispatcher
                auto future = dispatcher->submit(task);
                auto result = future.get();
                
                // Send result
                sendResult(result);
                
                state_machine_.processEvent(StateEvent::TASK_COMPLETED);
                
            } else if (code == 0) {
                // No tasks
                Logger::debug("No tasks available", "", "");
                
            } else if (code == -2) {
                // Invalid access code
                Logger::error("Invalid access code, re-registering");
                state_machine_.processEvent(StateEvent::REGISTER_FAIL);
            }
            
        } catch (const std::exception& e) {
            Logger::error("Failed to parse task response: " + std::string(e.what()));
        }
        
        // Wait before next poll
        std::this_thread::sleep_for(std::chrono::seconds(polling.base_interval_sec));
    }
    
    void sendResult(const Result& result) {
        ResultSender sender(Config::getInstance().getServer());
        
        if (sender.send(result)) {
            Logger::info("Result sent successfully", result.task_id, result.session_id);
        } else {
            Logger::error("Failed to send result", result.task_id, result.session_id);
            // Queue for retry
            failed_results_.push_back(result);
        }
    }
    
    void applyBackoff() {
        auto& polling = Config::getInstance().getPolling();
        current_backoff_ = std::min(current_backoff_ * polling.backoff_multiplier, 
                                   static_cast<double>(polling.max_interval_sec));
        std::this_thread::sleep_for(std::chrono::seconds(static_cast<int>(current_backoff_)));
    }
    
    void resetBackoff() {
        current_backoff_ = static_cast<double>(Config::getInstance().getPolling().base_interval_sec);
    }
    
    void handleError() {
        Logger::warning("Agent in error state, attempting recovery");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        state_machine_.processEvent(StateEvent::RETRY);
    }
    
    void cleanup() {
        Logger::info("Cleaning up...");
        
        // Clean temporary files
        const auto& exec = Config::getInstance().getExecution();
        std::error_code ec;
        std::filesystem::remove_all(exec.temp_dir, ec);
        
        // Retry failed results
        for (const auto& result : failed_results_) {
            ResultSender sender(Config::getInstance().getServer());
            sender.send(result);
        }
        
        state_machine_.processEvent(StateEvent::SHUTDOWN);
        Logger::info("Agent shutdown complete");
    }
    
    HttpClient http_;
    StateMachine state_machine_;
    std::vector<Result> failed_results_;
    double current_backoff_ = Config::getInstance().getPolling().base_interval_sec;
};

int main(int argc, char* argv[]) {
    try {
        Agent agent;
        if (!agent.initialize()) {
            std::cerr << "Failed to initialize agent" << std::endl;
            return 1;
        }
        
        agent.run();
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
