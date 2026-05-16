#pragma once

#include <memory>

#include "enum-utils.h"
#include "thread-work.h"

namespace Thread
{
    enum class TaskHandle : uint64_t
    {
        Sentinel = sentinel_for<TaskHandle>
    };

    enum class TaskDurationMS : uint64_t { };

    struct TaskResult
    {
        void* task_data; // Not null if completed.
        TaskDurationMS ms;
        bool being_cancelled;
    };

    class ThreadPool
    {
    public:
        struct Data;

        ThreadPool();
        ~ThreadPool();

        // Init.
        void startup(Arena::Arena* arena);

        // Shutdown.
        void shutdown();

        // Async notification.
        void async_notify();

        // Queuing tasks.
        TaskHandle background_task(void* task_data, ThreadWorkFn work_fn);

        // Retrieving results.
        TaskResult result_if_complete(TaskHandle task);

        // Task cancellation.
        void cancel_task(TaskHandle task);

        // Queries.
        uint64_t thread_count() const;
    private:
        std::unique_ptr<Data> data;
    };

    // Global setup.
    void set_system_thread_pool(ThreadPool* pool);

    // Global helpers.
    ThreadPool* system_thread_pool();
} // namespace Thread