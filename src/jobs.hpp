#pragma once
#include "common.hpp"
#include <thread>
namespace codm {
struct Task {
    std::string name;
    Clock::time_point created = Clock::now();
    JobContext context;
    std::atomic_bool done = false;
    bool started = false;
    std::function<void(JobContext &)> work;
    std::function<void()> onCancel;
    std::thread worker;
    ~Task() {
        context.cancel = true;
        if (worker.joinable())
            worker.join();
    }
};
// The UI owns the queue. Workers receive only their context and captured data.
class JobQueue {
    size_t limit;

  public:
    std::vector<std::unique_ptr<Task>> tasks;
    explicit JobQueue(size_t capacity = 3) : limit(capacity) {
        require(limit > 0, "Empty worker pool");
    }
    ~JobQueue() { shutdown(); }
    void enqueue(std::string name, std::function<void(JobContext &)> work) {
        auto task = std::make_unique<Task>();
        task->name = std::move(name);
        task->work = std::move(work);
        task->context.label = "Queued";
        tasks.push_back(std::move(task));
        poll();
    }
    void poll() {
        for (auto it = tasks.begin(); it != tasks.end();)
            if ((*it)->done)
                it = tasks.erase(it);
            else
                ++it;
        size_t running = 0;
        bool exporting = false;
        for (auto &t : tasks)
            if (t->started) {
                running++;
                exporting |= t->name == "Export assets";
            }
        for (auto &t : tasks) {
            if (t->started)
                continue;
            if (t->context.cancel) {
                t->done = true;
                continue;
            }
            if (running >= limit)
                break;
            if (t->name == "Export assets" && exporting)
                continue;
            t->started = true;
            running++;
            exporting |= t->name == "Export assets";
            auto *task = t.get();
            task->worker = std::thread([task] {
                task->work(task->context);
                task->done = true;
            });
        }
    }
    void shutdown() {
        for (auto &t : tasks)
            t->context.cancel = true;
        tasks.clear();
    }
};
} // namespace codm
