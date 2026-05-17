#pragma once

#include "models/Task.h"
#include "models/Result.h"
#include "TaskHandler.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <future>

namespace agent {

class TaskDispatcher {
public:
    explicit TaskDispatcher(size_t thread_count = 4);
    ~TaskDispatcher();
    
    // Submit task for async execution
    std::future<Result> submit(const Task& task);
    
    // Process task immediately (blocking)
    Result process(const Task& task);
    
    void stop();
    size_t pendingTasks() const;
    
private:
    struct TaskWrapper {
        Task task;
        std::promise<Result> promise;
    };
    
    void workerLoop();
    
    std::queue<TaskWrapper> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::vector<std::thread> workers_;
    bool running_ = true;
};

} // namespace agent
