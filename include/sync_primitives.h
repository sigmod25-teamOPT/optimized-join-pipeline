#pragma once

#include <condition_variable>
#include <task_queue.h>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <task.h>
#include <defs.h>

class TaskQueue;
class LoadTask;
class BatchLoadTask;
class JoinDispatcher;

// Atomically increments [atomic] and returns true if the new value is equal to [compare]
// In case of concurrent calls, only one will return true
static inline bool atomic_inc_and_cmp(std::atomic<uint32_t>& atomic, uint32_t compare) {
    auto old = atomic.load(std::memory_order_relaxed);
    while (!atomic.compare_exchange_weak(old, old + 1, std::memory_order_release, std::memory_order_relaxed));
    assert(old < compare);
    return old + 1 == compare;
}

class WaitObject {
    public:
        WaitObject() = default;

        void notify_all() {
            std::lock_guard<std::mutex> lock(mutex);
            condition = true;
            cond_var.notify_all();
        }

        void notify_one() {
            std::lock_guard<std::mutex> lock(mutex);
            condition = true;
            cond_var.notify_one();
        }

        void wait() {
            std::unique_lock<std::mutex> lock(mutex);
            cond_var.wait(lock, [this] { return condition; } );
        }

    private:
        std::mutex mutex;
        std::condition_variable cond_var;
        bool condition = false;
};

class SpinLock {
    public:
        SpinLock() = default;

        SpinLock(const SpinLock&) = delete;
        SpinLock& operator=(const SpinLock&) = delete;
        SpinLock(SpinLock&&) = delete;
        SpinLock& operator=(SpinLock&&) = delete;
        
        void lock() {
            while (flag.test_and_set(std::memory_order_acquire)) { }
        }
        
        void unlock() {
            flag.clear(std::memory_order_release);
        }   

    private:
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
};  


/*  Automatically gets lock on creation which is dropped
    when wait_for_join_loads() is called */
class LoadDispatcherState {
    public:
        LoadDispatcherState() {
            deferred_tasks.reserve(500);
            shared_lock.lock();
        }

        // Should only be called while holding mutex
        void register_load_task(std::shared_ptr<Task> task, size_t num_rows, ColumnUse col_use) {
            if(num_rows < MIN_ROWS_PER_JOB) {
                if(col_use == JOIN)
                    batch_task(std::move(task), num_rows);
                else
                    batch_task_deferred(std::move(task), num_rows);

                return;
            }

            if(col_use == JOIN) {
                tasks_registered++;
                TaskQueue::get_instance()->addTask(std::move(task));
            } else 
                deferred_tasks.push_back(std::move(task));
        }

        void signal_task_done() {
            std::shared_lock<std::shared_mutex> lock(shared_lock);
            if(atomic_inc_and_cmp(tasks_completed, tasks_registered))
                wait_object.notify_all();
        }

        void wait_for_join_loads() {
            volatile bool should_sleep = tasks_registered > 0;
            shared_lock.unlock();

            // Dump last incomplete batches
            if (current_batch.size() > 0) {
                auto task = std::static_pointer_cast<Task>(std::make_shared<BatchLoadTask>(std::move(current_batch), *this, 0));
                task->run_no_dispatch();
            }

            if (current_batch_deferred.size() > 0) {
                deferred_tasks.emplace_back(
                    std::make_shared<BatchLoadTask>(std::move(current_batch_deferred), *this, 0)
                );
            }

            if(should_sleep)
                wait_object.wait();
        }

        void start_deferred_tasks(std::shared_ptr<JoinDispatcher> root);

        int num_deferred_tasks() const {
            return deferred_tasks.size();
        }

    private:
        void batch_task(std::shared_ptr<Task> task, size_t num_rows) {
            current_batch.emplace_back(std::move(task));
            batched_rows += num_rows;

            if (batched_rows > ROWS_PER_JOB) {
                TaskQueue::get_instance()->addTask(
                    std::static_pointer_cast<Task>(
                        std::make_shared<BatchLoadTask>(std::move(current_batch), *this, 0)
                    )
                );

                tasks_registered++;
                
                current_batch.clear();
                batched_rows = 0;
            }
        }

        void batch_task_deferred(std::shared_ptr<Task> task, size_t num_rows) {
            current_batch_deferred.emplace_back(std::move(task));
            batched_rows_deferred += num_rows;

            if (batched_rows_deferred > ROWS_PER_JOB) {
                deferred_tasks.emplace_back(
                    std::make_shared<BatchLoadTask>(std::move(current_batch_deferred), *this, 0)
                );
 
                current_batch_deferred.clear();
                batched_rows_deferred = 0;
            }
        }

        std::shared_mutex shared_lock;

        WaitObject wait_object;

        uint32_t tasks_registered = 0;
        std::atomic<uint32_t> tasks_completed{0};

        std::vector<std::shared_ptr<Task>> current_batch;
        size_t batched_rows = 0;

        std::vector<std::shared_ptr<Task>> current_batch_deferred;
        size_t batched_rows_deferred = 0;

        std::vector<std::shared_ptr<Task>> deferred_tasks;
};