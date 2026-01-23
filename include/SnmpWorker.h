#ifndef SNMP_WORKER_H
#define SNMP_WORKER_H

#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct SnmpTask {
    std::string host;
    std::string community;
    std::string oid;
    std::function<void(const std::string&, const std::string&)> callback;
};

class SnmpWorker {
public:
    explicit SnmpWorker(size_t numThreads = 4);
    ~SnmpWorker();

    SnmpWorker(const SnmpWorker&) = delete;
    SnmpWorker& operator=(const SnmpWorker&) = delete;

    void start();
    void stop();
    void addTask(const SnmpTask& task);
    void wait();

private:
    void workerThread();
    bool getNextTask(SnmpTask& task);

    std::vector<std::thread> threads_;
    std::queue<SnmpTask> taskQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::atomic<bool> running_;
    std::atomic<size_t> activeTasks_;
    std::condition_variable completionCondition_;
    size_t numThreads_;
};

#endif
