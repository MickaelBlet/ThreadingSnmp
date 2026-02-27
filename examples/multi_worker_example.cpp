#include "SnmpWorker.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

std::atomic<int> getResponseCount(0);
std::atomic<int> informResponseCount(0);

int main(int argc, char* argv[]) {
    SOCK_STARTUP;
    init_snmp("MultiWorkerExample");

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <host> [community]" << std::endl;
        return 1;
    }

    std::string host = argv[1];
    std::string community = argc > 2 ? argv[2] : "public";

    std::cout << "=== Multiple SnmpWorker Example ===" << std::endl;
    std::cout << "Demonstrating concurrent INFORM and GET operations using separate workers" << std::endl;
    std::cout << std::endl;

    // Worker 1: Dedicated to INFORM operations
    SnmpWorker informWorker;
    informWorker.start();

    // Worker 2: Dedicated to GET operations
    SnmpWorker getWorker;
    getWorker.start();

    std::cout << "Both workers started. Launching tasks..." << std::endl << std::endl;

    // Launch multiple INFORM tasks on worker 1
    std::cout << "Launching INFORM tasks on worker 1..." << std::endl;
    for (int i = 0; i < 3; i++) {
        SnmpTask informTask;
        informTask.host = host;
        informTask.community = community;
        informTask.operation = SnmpOperation::INFORM;
        informTask.trapOid = "1.3.6.1.4.1.8072.2.3.0.1";
        informTask.informVarbinds = {
            {"1.3.6.1.4.1.8072.2.3.2.1", 'i', std::to_string(1000 + i)},
            {"1.3.6.1.4.1.8072.2.3.2.2", 's', "INFORM from worker 1 #" + std::to_string(i)}
        };
        informTask.informCallback = [i](bool success, const std::string& message) {
            if (success) {
                std::cout << "[INFORM Worker] Task " << i << " succeeded: " << message << std::endl;
            } else {
                std::cout << "[INFORM Worker] Task " << i << " failed: " << message << std::endl;
            }
            informResponseCount.fetch_add(1);
        };
        informWorker.addTask(informTask);
    }

    // Launch multiple GET tasks on worker 2 (concurrently with INFORMs)
    std::cout << "Launching GET tasks on worker 2..." << std::endl;
    std::vector<std::string> oids = {
        "1.3.6.1.2.1.1.1.0",
        "1.3.6.1.2.1.1.3.0",
        "1.3.6.1.2.1.1.5.0",
        "1.3.6.1.2.1.1.6.0"
    };

    for (size_t i = 0; i < oids.size(); i++) {
        SnmpTask getTask;
        getTask.host = host;
        getTask.community = community;
        getTask.oids = {oids[i]};
        getTask.callback = [i, &oids](const std::vector<std::pair<std::string, netsnmp_variable_list*>>& results) {
            if (results.empty()) {
                std::cout << "[GET Worker] Task " << i << " (" << oids[i] << ") failed" << std::endl;
            } else {
                for (const auto& [oid, var] : results) {
                    char valBuf[1024];
                    snprint_value(valBuf, sizeof(valBuf), var->name, var->name_length, var);
                    std::cout << "[GET Worker] Task " << i << " - " << oid << " -> " << valBuf << std::endl;
                }
            }
            getResponseCount.fetch_add(1);
        };
        getWorker.addTask(getTask);
    }

    std::cout << std::endl << "Waiting for all tasks to complete..." << std::endl;

    // Wait for both workers to complete their tasks
    informWorker.wait();
    getWorker.wait();

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << "INFORM responses: " << informResponseCount.load() << " / 3" << std::endl;
    std::cout << "GET responses: " << getResponseCount.load() << " / " << oids.size() << std::endl;

    // Stop both workers
    informWorker.stop();
    getWorker.stop();

    std::cout << std::endl << "Both workers stopped successfully!" << std::endl;
    std::cout << std::endl << "Note: Multiple SnmpWorker instances can run concurrently." << std::endl;
    std::cout << "Each worker has its own select thread and manages its own sessions." << std::endl;
    std::cout << "snmp_select_info() is thread-safe and can be called from multiple threads." << std::endl;

    return 0;
}
