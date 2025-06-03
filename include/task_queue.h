#pragma once

#include <queue>
#include <stddef.h>
#include <task.h>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cassert>

extern thread_local int worker_id;

class TaskQueue {
    public:
        TaskQueue();
        TaskQueue(size_t num_workers);
        ~TaskQueue();

        TaskQueue(const TaskQueue&) = delete;
        TaskQueue& operator=(const TaskQueue&) = delete;
        TaskQueue(TaskQueue&&) = delete;
        TaskQueue& operator=(TaskQueue&&) = delete;

        std::shared_ptr<Task> addTask(std::shared_ptr<Task> task);
        std::shared_ptr<Task> addTaskDirect(std::shared_ptr<Task> task);

        static TaskQueue* get_instance() {
            assert(TaskQueue::instance);
            return TaskQueue::instance;
        }

        size_t get_num_workers() const {
            return workers.size();
        }

    private:
        void workerThread(int slot);
        std::shared_ptr<Task> get_task_from_slot();

        std::vector<std::thread> workers;
        
        std::vector<size_t> worker_tasks;
        std::vector<std::shared_ptr<Task>> direct_assignment_slots;

        std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, CompareTask> tasks;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;

        bool done;

        static TaskQueue* instance;
};
