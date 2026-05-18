#pragma once
// thread_pool.h  —  потокобезопасная очередь + пул воркеров (header-only, C++17)

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include <stdexcept>

// ── SafeQueue ─────────────────────────────────────────────────────────────────

template<typename T>
class SafeQueue {
public:
    // Кладёт элемент и будит одного воркера
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Блокирует вызывающий поток пока очередь пуста или не придёт сигнал stop.
    // Возвращает false только после вызова stop() при пустой очереди.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mx_);
        cv_.wait(lk, [this]{ return !q_.empty() || stop_.load(); });
        if (stop_.load() && q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    // Будит все заблокированные pop() и запрещает дальнейшую блокировку
    void stop() {
        stop_.store(true);
        cv_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mx_);
        return q_.size();
    }

private:
    mutable std::mutex      mx_;
    std::condition_variable cv_;
    std::queue<T>           q_;
    std::atomic<bool>       stop_{false};
};

// ── ThreadPool ────────────────────────────────────────────────────────────────

class ThreadPool {
public:
    explicit ThreadPool(std::size_t n_workers) {
        workers_.reserve(n_workers);
        for (std::size_t i = 0; i < n_workers; ++i)
            workers_.emplace_back([this]{ run(); });
    }

    // Запрещаем копирование и перемещение
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() { shutdown(); }

    // Передать callable в очередь (не блокирует вызывающий поток)
    void submit(std::function<void()> fn) {
        if (stopped_.load())
            throw std::runtime_error("ThreadPool: submit after shutdown");
        queue_.push(std::move(fn));
    }

    // Дождаться завершения всех воркеров (graceful drain)
    void shutdown() {
        if (stopped_.exchange(true)) return;   // idempotent
        queue_.stop();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
    }

    std::size_t pending() const { return queue_.size(); }

private:
    void run() {
        std::function<void()> fn;
        while (queue_.pop(fn)) {
            try   { fn(); }
            catch (...) { /* исключения внутри задачи проглатываются;
                             обработчик сам логирует ошибку и вызывает send_result */ }
        }
    }

    SafeQueue<std::function<void()>> queue_;
    std::vector<std::thread>         workers_;
    std::atomic<bool>                stopped_{false};
};
