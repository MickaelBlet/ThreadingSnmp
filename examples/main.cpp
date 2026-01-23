#include "SnmpSession.h"
#include "SnmpWorker.h"
#include <iostream>
#include <chrono>
#include <atomic>

std::atomic<int> responseCount(0);

void printResult(const std::string& oid, const std::string& result) {
    std::cout << "OID: " << oid << " -> " << result << std::endl;
    responseCount.fetch_add(1);
}

int main(int argc, char* argv[]) {
    std::cout << "=== ThreadingSnmp Example ===" << std::endl;
    std::cout << "C++14 Net-SNMP Threading Demo" << std::endl << std::endl;

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <host> [community]" << std::endl;
        std::cout << "Example: " << argv[0] << " localhost public" << std::endl;
        std::cout << std::endl;
        std::cout << "Running demo with localhost..." << std::endl;
    }

    std::string host = (argc >= 2) ? argv[1] : "localhost";
    std::string community = (argc >= 3) ? argv[2] : "public";

    std::cout << "Host: " << host << std::endl;
    std::cout << "Community: " << community << std::endl << std::endl;

    std::cout << "=== Example 1: Single SNMP Session ===" << std::endl;
    {
        SnmpSession session(host, community);

        if (session.open()) {
            std::cout << "Session opened successfully" << std::endl;

            std::string sysDescr = session.get("1.3.6.1.2.1.1.1.0");
            std::cout << "sysDescr: " << sysDescr << std::endl;

            std::string sysUpTime = session.get("1.3.6.1.2.1.1.3.0");
            std::cout << "sysUpTime: " << sysUpTime << std::endl;

            std::string sysContact = session.get("1.3.6.1.2.1.1.4.0");
            std::cout << "sysContact: " << sysContact << std::endl;

            session.close();
        } else {
            std::cout << "Failed to open session" << std::endl;
        }
    }

    std::cout << std::endl << "=== Example 2: Multi-threaded SNMP Queries ===" << std::endl;
    {
        SnmpWorker worker(4);
        worker.start();

        std::vector<std::string> oids = {
            "1.3.6.1.2.1.1.1.0",
            "1.3.6.1.2.1.1.2.0",
            "1.3.6.1.2.1.1.3.0",
            "1.3.6.1.2.1.1.4.0",
            "1.3.6.1.2.1.1.5.0",
            "1.3.6.1.2.1.1.6.0",
            "1.3.6.1.2.1.1.7.0"
        };

        auto startTime = std::chrono::high_resolution_clock::now();

        for (const auto& oid : oids) {
            SnmpTask task;
            task.host = host;
            task.community = community;
            task.oid = oid;
            task.callback = printResult;
            worker.addTask(task);
        }

        std::cout << "Waiting for all tasks to complete..." << std::endl;
        worker.wait();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        std::cout << std::endl << "All tasks completed in " << duration.count() << " ms" << std::endl;
        std::cout << "Total responses: " << responseCount.load() << std::endl;

        worker.stop();
    }

    std::cout << std::endl << "=== Example 3: Multiple Hosts (if available) ===" << std::endl;
    {
        SnmpWorker worker(8);
        worker.start();

        std::vector<std::string> hosts = {host};

        for (const auto& h : hosts) {
            SnmpTask task;
            task.host = h;
            task.community = community;
            task.oid = "1.3.6.1.2.1.1.1.0";
            task.callback = [h](const std::string& oid, const std::string& result) {
                std::cout << "Host: " << h << " | OID: " << oid << " -> " << result << std::endl;
            };
            worker.addTask(task);
        }

        worker.wait();
        worker.stop();
    }

    std::cout << std::endl << "=== Demo Complete ===" << std::endl;

    return 0;
}
