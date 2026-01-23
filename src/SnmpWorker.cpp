#include "SnmpWorker.h"
#include <iostream>
#include <cstring>

SnmpWorker::SnmpWorker()
    : running_(false), activeTasks_(0) {
}

SnmpWorker::~SnmpWorker() {
    stop();
}

void SnmpWorker::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);
    selectThread_ = std::thread(&SnmpWorker::selectThread, this);
}

void SnmpWorker::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    queueCondition_.notify_all();

    if (selectThread_.joinable()) {
        selectThread_.join();
    }

    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& pair : activeSessions_) {
        if (pair.second->session) {
            snmp_close(pair.second->session);
            // Free the allocated strings
            free(pair.second->peername_allocated);
            free(pair.second->community_allocated);
        }
    }
    activeSessions_.clear();

    // Clean up any sessions that were queued for closing
    while (!sessionsToClose_.empty()) {
        SessionCleanup cleanup = sessionsToClose_.front();
        sessionsToClose_.pop();
        snmp_close(cleanup.session);
        free(cleanup.peername);
        free(cleanup.community);
    }
}

void SnmpWorker::addTask(const SnmpTask& task) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(task);
        activeTasks_.fetch_add(1);
    }
    queueCondition_.notify_one();
}

void SnmpWorker::addTask(const std::string& host,
                         const std::string& community,
                         const std::string& oid,
                         std::function<void(const std::string&, const std::string&)> callback,
                         int version,
                         long timeout,
                         int retries) {
    SnmpTask task;
    task.host = host;
    task.community = community;
    task.oid = oid;
    task.callback = callback;
    task.version = version;
    task.timeout = timeout;
    task.retries = retries;
    task.isMultiOid = false;
    addTask(task);
}

void SnmpWorker::addTask(const std::string& host,
                         const std::string& community,
                         const std::vector<std::string>& oids,
                         std::function<void(const std::vector<std::pair<std::string, std::string>>&)> multiCallback,
                         int version,
                         long timeout,
                         int retries) {
    SnmpTask task;
    task.host = host;
    task.community = community;
    task.oids = oids;
    task.multiCallback = multiCallback;
    task.version = version;
    task.timeout = timeout;
    task.retries = retries;
    task.isMultiOid = true;
    addTask(task);
}

void SnmpWorker::wait() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    completionCondition_.wait(lock, [this]() {
        std::lock_guard<std::mutex> sessionLock(sessionMutex_);
        return taskQueue_.empty() && activeTasks_.load() == 0 && activeSessions_.empty();
    });
}

int SnmpWorker::asyncCallback(int operation, netsnmp_session* session, int reqid, netsnmp_pdu* pdu, void* magic) {
    if (!magic) {
        return 1;
    }

    SnmpWorker* worker = static_cast<SnmpWorker*>(magic);
    std::shared_ptr<SessionContext> context;

    {
        std::lock_guard<std::mutex> lock(worker->sessionMutex_);
        auto it = worker->activeSessions_.find(session);
        if (it == worker->activeSessions_.end()) {
            return 1;
        }
        context = it->second;
    }

    if (operation == NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        if (pdu->errstat == SNMP_ERR_NOERROR) {
            if (context->task.isMultiOid) {
                std::vector<std::pair<std::string, std::string>> results;
                size_t idx = 0;
                for (netsnmp_variable_list* vars = pdu->variables;
                     vars != nullptr && idx < context->task.oids.size();
                     vars = vars->next_variable, ++idx) {
                    char buf[1024];
                    snprint_value(buf, sizeof(buf), vars->name, vars->name_length, vars);
                    results.push_back({context->task.oids[idx], std::string(buf)});
                }
                if (context->task.multiCallback) {
                    context->task.multiCallback(results);
                }
            } else {
                for (netsnmp_variable_list* vars = pdu->variables; vars != nullptr; vars = vars->next_variable) {
                    char buf[1024];
                    snprint_value(buf, sizeof(buf), vars->name, vars->name_length, vars);
                    if (context->task.callback) {
                        context->task.callback(context->task.oid, std::string(buf));
                    }
                }
            }
        } else {
            std::string errorMsg = "ERROR: " + std::string(snmp_errstring(pdu->errstat));
            if (context->task.isMultiOid) {
                std::vector<std::pair<std::string, std::string>> results;
                for (const auto& oid : context->task.oids) {
                    results.push_back({oid, errorMsg});
                }
                if (context->task.multiCallback) {
                    context->task.multiCallback(results);
                }
            } else {
                if (context->task.callback) {
                    context->task.callback(context->task.oid, errorMsg);
                }
            }
        }
    } else if (operation == NETSNMP_CALLBACK_OP_TIMED_OUT) {
        std::string errorMsg = "ERROR: Request timed out";
        if (context->task.isMultiOid) {
            std::vector<std::pair<std::string, std::string>> results;
            for (const auto& oid : context->task.oids) {
                results.push_back({oid, errorMsg});
            }
            if (context->task.multiCallback) {
                context->task.multiCallback(results);
            }
        } else {
            if (context->task.callback) {
                context->task.callback(context->task.oid, errorMsg);
            }
        }
    }

    context->completed = true;

    {
        std::lock_guard<std::mutex> lock(worker->sessionMutex_);
        if (context->session) {
            // Don't call snmp_close() here! We're still inside the net-snmp callback.
            // Queue it for cleanup after snmp_read() returns, along with allocated strings.
            SessionCleanup cleanup;
            cleanup.session = context->session;
            cleanup.peername = context->peername_allocated;
            cleanup.community = context->community_allocated;
            worker->sessionsToClose_.push(cleanup);
            context->session = nullptr;
        }
        worker->activeSessions_.erase(session);
    }

    worker->activeTasks_.fetch_sub(1);

    {
        std::lock_guard<std::mutex> lock(worker->queueMutex_);
        if (worker->taskQueue_.empty() && worker->activeTasks_.load() == 0) {
            worker->completionCondition_.notify_all();
        }
    }

    return 1;
}

void SnmpWorker::processTask(const SnmpTask& task) {
    netsnmp_session session;
    snmp_sess_init(&session);

    session.version = task.version;
    session.peername = strdup(task.host.c_str());
    session.community = reinterpret_cast<u_char*>(strdup(task.community.c_str()));
    session.community_len = task.community.length();
    session.callback = asyncCallback;
    session.callback_magic = this;
    session.timeout = task.timeout;
    session.retries = task.retries;

    netsnmp_session* sessionHandle = snmp_open(&session);

    // snmp_open() makes copies of peername and community, so we need to free our originals
    // Important: Only free AFTER snmp_read() completes, to avoid the callback crash
    char* peername_to_free = session.peername;
    u_char* community_to_free = session.community;

    if (!sessionHandle) {
        // If snmp_open failed, free the allocated memory immediately
        free(peername_to_free);
        free(community_to_free);
        std::string errorMsg = "ERROR: Failed to open session";
        if (task.isMultiOid) {
            std::vector<std::pair<std::string, std::string>> results;
            for (const auto& oid : task.oids) {
                results.push_back({oid, errorMsg});
            }
            if (task.multiCallback) {
                task.multiCallback(results);
            }
        } else {
            if (task.callback) {
                task.callback(task.oid, errorMsg);
            }
        }
        activeTasks_.fetch_sub(1);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (taskQueue_.empty() && activeTasks_.load() == 0) {
                completionCondition_.notify_all();
            }
        }
        return;
    }

    auto context = std::make_shared<SessionContext>();
    context->task = task;
    context->session = sessionHandle;
    context->completed = false;
    context->peername_allocated = peername_to_free;
    context->community_allocated = community_to_free;

    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        activeSessions_[sessionHandle] = context;
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GET);

    if (task.isMultiOid) {
        for (const auto& oidStr : task.oids) {
            oid oidArray[MAX_OID_LEN];
            size_t oidLen = MAX_OID_LEN;
            if (read_objid(oidStr.c_str(), oidArray, &oidLen)) {
                snmp_add_null_var(pdu, oidArray, oidLen);
            }
        }
    } else {
        oid oidArray[MAX_OID_LEN];
        size_t oidLen = MAX_OID_LEN;
        if (read_objid(task.oid.c_str(), oidArray, &oidLen)) {
            snmp_add_null_var(pdu, oidArray, oidLen);
        }
    }

    if (snmp_send(sessionHandle, pdu) == 0) {
        snmp_free_pdu(pdu);
        std::string errorMsg = "ERROR: Failed to send request";
        if (task.isMultiOid) {
            std::vector<std::pair<std::string, std::string>> results;
            for (const auto& oid : task.oids) {
                results.push_back({oid, errorMsg});
            }
            if (task.multiCallback) {
                task.multiCallback(results);
            }
        } else {
            if (task.callback) {
                task.callback(task.oid, errorMsg);
            }
        }

        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            // Free the allocated strings before closing the session
            free(peername_to_free);
            free(community_to_free);
            snmp_close(sessionHandle);
            activeSessions_.erase(sessionHandle);
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

void SnmpWorker::selectThread() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queueMutex_);

        if (!taskQueue_.empty()) {
            SnmpTask task = taskQueue_.front();
            taskQueue_.pop();
            lock.unlock();

            processTask(task);
        } else {
            queueCondition_.wait_for(lock, std::chrono::milliseconds(10));
            lock.unlock();
        }

        int fds = 0;
        fd_set fdset;
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        int block = 0;

        FD_ZERO(&fdset);
        snmp_select_info(&fds, &fdset, &timeout, &block);

        if (fds > 0) {
            int count = select(fds, &fdset, nullptr, nullptr, block ? nullptr : &timeout);
            if (count > 0) {
                snmp_read(&fdset);
            } else if (count == 0) {
                snmp_timeout();
            }

            // Now it's safe to close sessions that were marked for cleanup
            // This must happen after both snmp_read() and snmp_timeout()
            std::lock_guard<std::mutex> sessionLock(sessionMutex_);
            while (!sessionsToClose_.empty()) {
                SessionCleanup cleanup = sessionsToClose_.front();
                sessionsToClose_.pop();

                // Close the session and free the allocated strings
                snmp_close(cleanup.session);
                free(cleanup.peername);
                free(cleanup.community);
            }
        }
    }
}
