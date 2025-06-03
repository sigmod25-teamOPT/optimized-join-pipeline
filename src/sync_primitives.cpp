#include <sync_primitives.h>
#include <task_column_load.h>

void LoadDispatcherState::start_deferred_tasks(std::shared_ptr<JoinDispatcher> root) {
    for(auto& task : deferred_tasks) {
        auto load_task = std::static_pointer_cast<LoadTask>(task);
        assert(load_task);
        load_task->set_root(root);
        TaskQueue::get_instance()->addTask(load_task);
    }
}