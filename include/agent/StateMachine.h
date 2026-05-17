#pragma once

#include <string>
#include <functional>
#include <chrono>

namespace agent {

enum class AgentState {
    INITIALIZING,
    REGISTERING,
    REGISTERED,
    POLLING,
    EXECUTING_TASK,
    SENDING_RESULT,
    ERROR,
    SHUTTING_DOWN,
    SHUTDOWN
};

enum class StateEvent {
    INIT_OK,
    REGISTER_OK,
    REGISTER_FAIL,
    TASK_RECEIVED,
    TASK_COMPLETED,
    TASK_FAILED,
    NETWORK_ERROR,
    SHUTDOWN,
    RETRY
};

class StateMachine {
public:
    using StateCallback = std::function<void(AgentState, const std::string&)>;
    
    StateMachine();
    
    void processEvent(StateEvent event, const std::string& context = "");
    AgentState getCurrentState() const;
    void setStateCallback(StateCallback callback);
    bool isOperational() const;
    
private:
    AgentState current_state_;
    StateCallback callback_;
    std::chrono::steady_clock::time_point last_state_change_;
    
    void transitionTo(AgentState new_state, const std::string& reason);
};

} // namespace agent
