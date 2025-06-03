#include <task_queue.h>
#include <numeric>

TaskQueue* TaskQueue::instance = nullptr;
thread_local int worker_id = -1;

TaskQueue::TaskQueue(): TaskQueue(std::thread::hardware_concurrency()) {}

TaskQueue::TaskQueue(size_t num_workers) {
    // Grab mutex so no work can start before
    // initialization is finished
    std::lock_guard<std::mutex> lock(queue_mutex);

    std::cout << "Creating task queue with " << num_workers << " workers" << std::endl;
    done = false;

    workers.reserve(num_workers);
    direct_assignment_slots.resize(num_workers, nullptr);
    worker_tasks.resize(num_workers, 0);

    for (size_t i = 0; i < num_workers; i++)
        workers.emplace_back([this, i] { workerThread(i); });

    assert(direct_assignment_slots.size() == num_workers);

    TaskQueue::instance = this;
}

TaskQueue::~TaskQueue() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        done = true;
    }

    queue_cv.notify_all();

    for (std::thread& worker: workers) {
        assert(worker.joinable());
        worker.join();
    }
}

std::shared_ptr<Task> TaskQueue::addTask(std::shared_ptr<Task> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        tasks.push(task);
    }

    queue_cv.notify_one();
    return task;
}

std::shared_ptr<Task> TaskQueue::addTaskDirect(std::shared_ptr<Task> task) {
    // If call didn't come from a worker thread
    // add task to queue the normal way.
    if(worker_id < 0)
        return addTask(task);

    // Otherwise add it to the task slot
    // allocated for the current thread
    assert(worker_id >= 0);
    assert(worker_id < direct_assignment_slots.size());

    if(direct_assignment_slots[worker_id])
        return addTask(task);

    assert(!direct_assignment_slots[worker_id]);
    direct_assignment_slots[worker_id] = task;
    return task;
}

void TaskQueue::workerThread(int slot) {
    assert(slot >= 0);
    assert(slot < direct_assignment_slots.size());

    worker_id = slot;

    while (true) {
        std::shared_ptr<Task> task = get_task_from_slot();

        if(!task) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this] { return done || !tasks.empty(); });

            if (done && tasks.empty())
                return;

            task = tasks.top();
            tasks.pop();
        }
        worker_tasks[worker_id]++;
        task->run();
    }
}

std::shared_ptr<Task> TaskQueue::get_task_from_slot() {
    std::shared_ptr<Task> result = std::move(direct_assignment_slots[worker_id]);
    if(result)
        direct_assignment_slots[worker_id] = nullptr;

    return result;
}