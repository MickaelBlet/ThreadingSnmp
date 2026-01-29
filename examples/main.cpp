#include "SnmpSession.h"
#include "SnmpWorker.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <thread>

std::atomic<int> responseCount(0);

int main(int argc, char* argv[]) {
    SOCK_STARTUP;
    init_snmp("ThreadingSnmp");

    std::cout << "=== ThreadingSnmp Example ===" << std::endl;
    std::cout << "C++14 Net-SNMP Threading Demo with snmp_select" << std::endl << std::endl;

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

    std::cout << std::endl << "=== Example 2: Multi-OID GET in Single Request ===" << std::endl;
    {
        SnmpSession session(host, community);

        if (session.open()) {
            std::cout << "Session opened successfully" << std::endl;

            std::vector<std::string> oids = {
                "1.3.6.1.2.1.1.1.0",
                "1.3.6.1.2.1.1.2.0",
                "1.3.6.1.2.1.1.3.0",
                "1.3.6.1.2.1.1.4.0",
                "1.3.6.1.2.1.1.5.0"
            };

            auto startTime = std::chrono::high_resolution_clock::now();
            auto results = session.getMulti(oids);
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            std::cout << "Retrieved " << results.size() << " OIDs in " << duration.count() << " ms:" << std::endl;
            for (const auto& result : results) {
                std::cout << "  " << result.first << " -> " << result.second << std::endl;
            }

            session.close();
        } else {
            std::cout << "Failed to open session" << std::endl;
        }
    }

    std::cout << std::endl << "=== Example 3: Async Single OID Requests ===" << std::endl;
    {
        SnmpWorker worker;
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

        // Each task requests one OID
        for (const auto& oid : oids) {
            SnmpTask task;
            task.host = host;
            task.community = community;
            task.oids = {oid};  // Single OID in vector
            task.callback = [](const std::vector<std::pair<std::string, std::string>>& results) {
                for (const auto& result : results) {
                    std::cout << "OID: " << result.first << " -> " << result.second << std::endl;
                }
            };
            worker.addTask(task);
        }

        std::cout << "Waiting for all async tasks to complete..." << std::endl;
        worker.wait();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        std::cout << std::endl << "All async tasks completed in " << duration.count() << " ms" << std::endl;

        worker.stop();
    }

    std::cout << std::endl << "=== Example 4: Async Multi-OID GET ===" << std::endl;
    {
        responseCount.store(0);
        SnmpWorker worker;
        worker.start();

        std::vector<std::string> group1 = {
            "1.3.6.1.2.1.1.1.0",
            "1.3.6.1.2.1.1.2.0",
            "1.3.6.1.2.1.1.3.0"
        };

        std::vector<std::string> group2 = {
            "1.3.6.1.2.1.1.4.0",
            "1.3.6.1.2.1.1.5.0",
            "1.3.6.1.2.1.1.6.0"
        };

        auto startTime = std::chrono::high_resolution_clock::now();

        SnmpTask task1;
        task1.host = host;
        task1.community = community;
        task1.oids = group1;
        task1.callback = [](const std::vector<std::pair<std::string, std::string>>& results) {
            std::cout << "Group 1 response:" << std::endl;
            for (const auto& result : results) {
                std::cout << "  " << result.first << " -> " << result.second << std::endl;
            }
            responseCount.fetch_add(1);
        };
        worker.addTask(task1);

        SnmpTask task2;
        task2.host = host;
        task2.community = community;
        task2.oids = group2;
        task2.callback = [](const std::vector<std::pair<std::string, std::string>>& results) {
            std::cout << "Group 2 response:" << std::endl;
            for (const auto& result : results) {
                std::cout << "  " << result.first << " -> " << result.second << std::endl;
            }
            responseCount.fetch_add(1);
        };
        worker.addTask(task2);

        std::cout << "Waiting for all multi-OID async tasks to complete..." << std::endl;
        worker.wait();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        std::cout << std::endl << "All multi-OID async tasks completed in " << duration.count() << " ms" << std::endl;
        std::cout << "Total responses: " << responseCount.load() << std::endl;

        worker.stop();
    }

    std::cout << std::endl << "=== Example 5: Async SNMP SET Operation ===" << std::endl;
    {
        SnmpWorker worker;
        worker.start();

        SnmpTask setTask;
        setTask.host = host;
        setTask.community = community;
        setTask.operation = SnmpOperation::SET;
        setTask.setValues = {
            {"1.3.6.1.2.1.1.4.0", 's', "admin@example.com"},
            {"1.3.6.1.2.1.1.6.0", 's', "Server Room A"}
        };
        setTask.setCallback = [](bool success, const std::string& message) {
            if (success) {
                std::cout << "SET operation succeeded: " << message << std::endl;
            } else {
                std::cout << "SET operation failed: " << message << std::endl;
            }
        };

        worker.addTask(setTask);
        worker.wait();
        worker.stop();

        std::cout << "Note: SET operations may fail if the SNMP agent is read-only" << std::endl;
    }

    std::cout << std::endl << "=== Example 6: SNMP Trap Receiver ===" << std::endl;
    {
        std::cout << "To demonstrate trap receiver, send a trap:" << std::endl;
        std::cout << "Example: snmptrap -v 2c -c public localhost '' 1.3.6.1.4.1.8072.2.3.0.1 1.3.6.1.4.1.8072.2.3.2.1 i 123456" << std::endl;
        std::cout << std::endl;

        SnmpWorker worker;
        worker.start();

        auto trapHandler = [](const SnmpTrap& trap) {
            std::cout << std::endl << "=== TRAP RECEIVED ===" << std::endl;
            std::cout << "Source: " << trap.sourceIp << std::endl;
            std::cout << "Community: " << trap.community << std::endl;
            if (!trap.enterpriseOid.empty()) {
                std::cout << "Enterprise OID: " << trap.enterpriseOid << std::endl;
                std::cout << "Generic Trap: " << trap.genericTrap << std::endl;
                std::cout << "Specific Trap: " << trap.specificTrap << std::endl;
                std::cout << "Uptime: " << trap.uptime << std::endl;
            }
            std::cout << "Varbinds:" << std::endl;
            for (const auto& vb : trap.varbinds) {
                std::cout << "  " << vb.first << " = " << vb.second << std::endl;
            }
            std::cout << "===================" << std::endl;
        };

        std::cout << "Starting trap receiver (will listen for 10 seconds)..." << std::endl;
        worker.startTrapReceiver(162, trapHandler);

        std::this_thread::sleep_for(std::chrono::seconds(10));

        worker.stopTrapReceiver();
        worker.stop();
        std::cout << "Trap receiver example complete" << std::endl;
    }

    std::cout << std::endl << "=== Example 7: SNMP INFORM Operation ===" << std::endl;
    {
        SnmpWorker worker;
        worker.start();

        SnmpTask informTask;
        informTask.host = host;
        informTask.community = community;
        informTask.operation = SnmpOperation::INFORM;
        informTask.trapOid = "1.3.6.1.4.1.8072.2.3.0.1";

        informTask.informVarbinds = {
            {"1.3.6.1.4.1.8072.2.3.2.1", 'i', "12345"},
            {"1.3.6.1.4.1.8072.2.3.2.2", 's', "Test Inform"}
        };

        informTask.informCallback = [](bool success, const std::string& message) {
            if (success) {
                std::cout << "INFORM operation succeeded: " << message << std::endl;
            } else {
                std::cout << "INFORM operation failed: " << message << std::endl;
            }
        };

        worker.addTask(informTask);
        worker.wait();
        worker.stop();

        std::cout << "Note: INFORM requires an SNMP manager to acknowledge the notification" << std::endl;
        std::cout << "Unlike TRAPs, INFORMs are acknowledged and retransmitted if lost" << std::endl;
    }

    std::cout << std::endl << "=== Demo Complete ===" << std::endl;
    std::cout << std::endl;
    std::cout << "Summary:" << std::endl;
    std::cout << "- SnmpSession supports both single OID and multi-OID GET" << std::endl;
    std::cout << "- SnmpWorker uses snmp_select for efficient async I/O" << std::endl;
    std::cout << "- Single thread handles all SNMP requests using select()" << std::endl;
    std::cout << "- Unified API: always use vector for OIDs, single callback type" << std::endl;
    std::cout << "- Async SET operations for modifying SNMP values" << std::endl;
    std::cout << "- Async INFORM operations with acknowledgment" << std::endl;
    std::cout << "- Built-in SNMP trap receiver for monitoring notifications" << std::endl;
    std::cout << "- Better performance with concurrent requests" << std::endl;

    snmp_shutdown("ThreadingSnmp");
    shutdown_mib();
    netsnmp_container_free_list();
    SOCK_CLEANUP;
    return 0;
}
