#include "thread.h"

#include <cassert>

#include "file-tracker.h"
#include "list-helpers.h"
#include "thread-ctx.h"
#include "timers.h"

namespace Thread
{
    namespace
    {
        struct ThreadPoolArray
        {
            OS::Thread* threads;
            uint64_t count;
        };

        struct TaskResultEntry;

        struct TaskListEntry
        {
            TaskListEntry* next;
            TaskResultEntry* result;
        };

        struct TaskList
        {
            TaskListEntry* first;
            TaskListEntry* last;
            TaskListEntry* free_lst;
            uint64_t count;
        };

        struct TaskResultEntry
        {
            TaskResultEntry* next;
            TaskResultEntry* prev;
            ThreadWorkData thread_work_data;
            uint32_t completion_flag;
        };

        struct TaskResultList
        {
            TaskResultEntry* first;
            TaskResultEntry* last;
            uint64_t count;
        };

        struct TaskResults
        {
            TaskResultList results;
            TaskResultEntry* free_lst;
        };

        TaskHandle thread_task_as_handle(TaskResultEntry* h)
        {
            return TaskHandle{ reinterpret_cast<uint64_t>(h) };
        }

        TaskResultEntry* thread_handle_as_task(TaskHandle h)
        {
            return reinterpret_cast<TaskResultEntry*>(h);
        }

        void push_task(Arena::Arena* arena, TaskList* lst, TaskResultEntry* result)
        {
            TaskListEntry* entry = nullptr;
            if (lst->free_lst != nullptr)
            {
                entry = lst->free_lst;
                SLLStackPop(lst->free_lst);
                zero_bytes(entry);
            }
            else
            {
                entry = Arena::push_array<TaskListEntry>(arena, 1);
            }
            entry->result = result;
            SLLQueuePush(lst->first, lst->last, entry);
            ++lst->count;
        }

        void pop_task(TaskList* lst)
        {
            assert(lst->count != 0);
            TaskListEntry* entry = lst->first;
            SLLQueuePop(lst->first, lst->last);
            --lst->count;
            SLLStackPush(lst->free_lst, entry);
        }

        bool any_work(const TaskList& lst)
        {
            return lst.count != 0;
        }

        TaskResultEntry* reserve_result_entry_slot(Arena::Arena* arena, TaskResults* results, ThreadWorkData data)
        {
            TaskResultEntry* entry = nullptr;
            if (results->free_lst != nullptr)
            {
                entry = results->free_lst;
                SLLStackPop(results->free_lst);
                zero_bytes(entry);
            }
            else
            {
                entry = Arena::push_array<TaskResultEntry>(arena, 1);
            }
            entry->thread_work_data = data;
            TaskResultList* lst = &results->results;
            DLLPushBack(lst->first, lst->last, entry);
            ++lst->count;
            return entry;
        }

        void release_result_entry(TaskResults* results, TaskResultEntry* entry)
        {
            TaskResultList* lst = &results->results;
            assert(lst->count != 0);
            DLLRemove(lst->first, lst->last, entry);
            --lst->count;
            entry->next = entry->prev = nullptr;
            SLLStackPush(results->free_lst, entry);
        }
    } // namespace [anon]

    struct ThreadPool::Data
    {
        ThreadPoolArray pool{};
        Arena::Arena* task_arena;
        Arena::Arena* result_arena;
        OS::Mutex queue_mutex = OS::Mutex::Sentinel;
        OS::ConditionVariable queue_condition = OS::ConditionVariable::Sentinel;
        OS::Mutex result_mutex = OS::Mutex::Sentinel;
        TaskList input_queue{};
        TaskResults results{};
        uint32_t execute_async = false;

        bool terminate = false;
    };

    namespace
    {
        void thread_work_core(void* data_p)
        {
            ThreadPool::Data* data = static_cast<ThreadPool::Data*>(data_p);
            while (true)
            {
                TaskListEntry entry = {};
                {
                    OS::lock_mutex(data->queue_mutex);
                    // Wait to be woken up again.
                    if (not any_work(data->input_queue) and not data->terminate)
                    {
                        do
                        {
                            OS::wait_condition_var(data->queue_condition, data->queue_mutex, OS::MicroSec::Infinite);
                            // If there's async work, do it first then perform the normal work check.
                            if (os_atomic_u32_eval_cond_assign(&data->execute_async, 0, 1))
                            {
                                FileTrack::async_map_tick();
                            }
                        } while (not any_work(data->input_queue) and not data->terminate);
                    }
                    if (data->terminate)
                    {
                        OS::unlock_mutex(data->queue_mutex);
                        return;
                    }
                    entry = *data->input_queue.first;
                    pop_task(&data->input_queue);
                    OS::unlock_mutex(data->queue_mutex);
                }
                // Do work.
                assert(entry.result != nullptr);
                Timers::Stopwatch sw;
                sw.start();
                entry.result->thread_work_data.work_fn(&entry.result->thread_work_data);
                sw.stop();
                // Update result slot.
                {
                    OS::lock_mutex(data->result_mutex);
                    TaskResultEntry* result_handle = entry.result;
                    result_handle->thread_work_data.sw = sw;
                    os_atomic_u32_eval_assign(&result_handle->completion_flag, 1);
                    OS::unlock_mutex(data->result_mutex);
                }
#if BUILD_DEBUG
                Arena::validate_scratch_arenas();
#endif // BUILD_DEBUG
            }
        }

        void startup(ThreadPool::Data* data, Arena::Arena* arena)
        {
            data->task_arena = Arena::alloc(Arena::default_params);
            data->result_arena = Arena::alloc(Arena::default_params);
            data->queue_mutex = OS::alloc_mutex();
            data->result_mutex = OS::alloc_mutex();
            data->queue_condition = OS::alloc_condition_var();
            const uint32_t thread_count = std::max(rep(OS::system_info()->processor_count), 2u);
            data->pool.threads = Arena::push_array_no_zero<OS::Thread>(arena, thread_count);
            data->pool.count = thread_count;
            for EachIndex(i, thread_count)
            {
                data->pool.threads[i] = OS::launch_thread(thread_work_core, data);
            }
        }

        ThreadPool* system_pool;
    } // namespace [anon]

    ThreadPool::ThreadPool():
        data{ new Data } { }

    ThreadPool::~ThreadPool() = default;

    void ThreadPool::startup(Arena::Arena* arena)
    {
        PROF_SCOPE();

        ::Thread::startup(data.get(), arena);
    }

    void ThreadPool::shutdown()
    {
        // We'll automatically shutdown by issuing terminate and joining all threads.
        {
            OS::lock_mutex(data->queue_mutex);
            data->terminate = true;
            OS::unlock_mutex(data->queue_mutex);
        }
        OS::notify_all_condition_var(data->queue_condition);
        for EachIndex(i, data->pool.count)
        {
            OS::join_thread(data->pool.threads[i]);
        }
        OS::release_mutex(data->queue_mutex);
        OS::release_mutex(data->result_mutex);
        OS::release_condition_var(data->queue_condition);
    }

    // Async notification.
    void ThreadPool::async_notify()
    {
        os_atomic_u32_eval_assign(&data->execute_async, 1);
        // Also wake up a thread to do the work.
        OS::notify_one_condition_var(data->queue_condition);
    }

    // Queuing tasks.
    TaskHandle ThreadPool::background_task(void* task_data, ThreadWorkFn work_fn)
    {
        TaskResultEntry* result = nullptr;
        {
            OS::lock_mutex(data->result_mutex);
            ThreadWorkData t_data = {
                .data = task_data,
                .work_fn = work_fn,
            };
            result = reserve_result_entry_slot(data->result_arena, &data->results, t_data);
            OS::unlock_mutex(data->result_mutex);
        }
        // Capture the work.
        {
            OS::lock_mutex(data->queue_mutex);
            push_task(data->task_arena, &data->input_queue, result);
            OS::unlock_mutex(data->queue_mutex);
        }
        // Start.
        OS::notify_one_condition_var(data->queue_condition);
        return thread_task_as_handle(result);
    }

    // Retrieving results.
    TaskResult ThreadPool::result_if_complete(TaskHandle task)
    {
        TaskResult result = {};
        assert(task != TaskHandle::Sentinel);
        TaskResultEntry* e = thread_handle_as_task(task);
        result.being_cancelled = os_atomic_u32_eval(&e->thread_work_data.cancellation_flag) != 0;
        if (os_atomic_u32_eval(&e->completion_flag) != 0)
        {
            // Lock and release.
            OS::lock_mutex(data->result_mutex);
            result.task_data = e->thread_work_data.data;
            result.ms = TaskDurationMS{ e->thread_work_data.sw.to_ms() };
            result.being_cancelled = false;
            // Release the task.
            release_result_entry(&data->results, e);
            OS::unlock_mutex(data->result_mutex);
        }
        return result;
    }

    // Task cancellation.
    void ThreadPool::cancel_task(TaskHandle task)
    {
        assert(task != TaskHandle::Sentinel);
        os_atomic_u32_eval_assign(&thread_handle_as_task(task)->thread_work_data.cancellation_flag, 1);
    }

    uint64_t ThreadPool::thread_count() const
    {
        return data->pool.count;
    }

    void set_system_thread_pool(ThreadPool* pool)
    {
        system_pool = pool;
    }

    ThreadPool* system_thread_pool()
    {
        return system_pool;
    }
} // namespace Thread