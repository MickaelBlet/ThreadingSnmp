#include "SnmpWorker.h"
#include "SnmpSession.h"
#include <iostream>

SnmpWorker::SnmpWorker(size_t numThreads)
    : running_(false), activeTasks_(0), numThreads_(numThreads) {
}

SnmpWorker::~SnmpWorker() {
    stop();
}

void SnmpWorker::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);

    for (size_t i = 0; i < numThreads_; ++i) {
        threads_.emplace_back(&SnmpWorker::workerThread, this);
    }
}

void SnmpWorker::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    queueCondition_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    threads_.clear();
}

void SnmpWorker::addTask(const SnmpTask& task) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(task);
        activeTasks_.fetch_add(1);
    }
    queueCondition_.notify_one();
}

void SnmpWorker::wait() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    completionCondition_.wait(lock, [this]() {
        return taskQueue_.empty() && activeTasks_.load() == 0;
    });
}

bool SnmpWorker::getNextTask(SnmpTask& task) {
    std::unique_lock<std::mutex> lock(queueMutex_);

    queueCondition_.wait(lock, [this]() {
        return !taskQueue_.empty() || !running_.load();
    });

    if (!running_.load() && taskQueue_.empty()) {
        return false;
    }

    if (!taskQueue_.empty()) {
        task = taskQueue_.front();
        taskQueue_.pop();
        return true;
    }

    return false;
}

void SnmpWorker::workerThread() {
    while (running_.load()) {
        SnmpTask task;

        if (!getNextTask(task)) {
            continue;
        }

        try {
            SnmpSession session(task.host, task.community);

            if (session.open()) {
                std::string result = session.get(task.oid);

                if (task.callback) {
                    task.callback(task.oid, result);
                }

                session.close();
            } else {
                if (task.callback) {
                    task.callback(task.oid, "ERROR: Failed to open session");
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Worker thread exception: " << e.what() << std::endl;
            if (task.callback) {
                task.callback(task.oid, "ERROR: Exception occurred");
            }
        }

        activeTasks_.fetch_sub(1);

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (taskQueue_.empty() && activeTasks_.load() == 0) {
                completionCondition_.notify_all();
            }
        }
    }
}
