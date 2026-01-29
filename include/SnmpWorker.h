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
#include <map>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

enum class SnmpOperation {
    GET,
    SET,
    INFORM
};

struct SnmpSetValue {
    std::string oid;
    char type;
    std::string value;
};

struct SnmpVarbind {
    std::string oid;
    char type;
    std::string value;
};

struct SnmpTask {
    std::string host;
    std::string community;
    std::vector<std::string> oids;  // Always use vector, even for single OID
    std::vector<SnmpSetValue> setValues;
    std::vector<SnmpVarbind> informVarbinds;
    std::string trapOid;
    std::function<void(const std::vector<std::pair<std::string, std::string>>&)> callback;  // Unified callback
    std::function<void(bool, const std::string&)> setCallback;
    std::function<void(bool, const std::string&)> informCallback;
    int version;
    SnmpOperation operation;
    long timeout;   // Timeout in microseconds
    int retries;    // Number of retries

    SnmpTask() : version(SNMP_VERSION_2c), operation(SnmpOperation::GET), timeout(1000000), retries(3) {}
};

struct SnmpTrap {
    std::string sourceIp;
    std::string community;
    std::string enterpriseOid;
    int genericTrap;
    int specificTrap;
    std::vector<std::pair<std::string, std::string>> varbinds;
    unsigned long uptime;
};

class SnmpWorker {
public:
    SnmpWorker();
    ~SnmpWorker();

    SnmpWorker(const SnmpWorker&) = delete;
    SnmpWorker& operator=(const SnmpWorker&) = delete;

    void start();
    void stop();
    void addTask(const SnmpTask& task);
    void wait();

    void startTrapReceiver(int port = 162, const std::function<void(const SnmpTrap&)>& callback = nullptr);
    void stopTrapReceiver();

private:
    struct SessionContext {
        SnmpTask task;
        netsnmp_session* session;
        bool completed;
        char* peername_allocated;
        u_char* community_allocated;
    };

    struct SessionCleanup {
        netsnmp_session* session;
        char* peername;
        u_char* community;
    };

    void selectThread();
    void processTask(const SnmpTask& task);
    static int asyncCallback(int operation, netsnmp_session* session, int reqid, netsnmp_pdu* pdu, void* magic);
    static int trapCallback(int operation, netsnmp_session* session, int reqid, netsnmp_pdu* pdu, void* magic);

    std::thread selectThread_;
    std::queue<SnmpTask> taskQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::atomic<bool> running_;
    std::atomic<bool> trapRunning_;
    std::atomic<size_t> activeTasks_;
    std::condition_variable completionCondition_;
    std::mutex sessionMutex_;
    std::map<void*, std::shared_ptr<SessionContext>> activeSessions_;
    std::queue<SessionCleanup> sessionsToClose_;
    std::function<void(const SnmpTrap&)> trapCallback_;
    netsnmp_session* trapSession_;
};

#endif
