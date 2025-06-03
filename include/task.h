#pragma once

#include <iostream>
#include <thread>
#include <condition_variable>
#include <mutex>

class Task {
    public:
        Task(int priority) : priority(priority) { }

        virtual ~Task() = default;

        void run() {
            run_task();
            post_run_clarity();
        }

        void run_no_dispatch() {
            run_task();
        }

        int get_priority() {
            return priority;
        }

    protected:
        virtual void run_task() = 0;
        virtual void post_run_clarity() {}

    private:
        int priority;
};

struct CompareTask {
    bool operator()(const std::shared_ptr<Task>& t1, const std::shared_ptr<Task>& t2) {
        // Higher number for higher priority
        return t1->get_priority() < t2->get_priority();
    }
};
