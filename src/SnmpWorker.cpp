#include "SnmpWorker.h"
#include <iostream>
#include <cstring>

SnmpWorker::SnmpWorker()
    : running_(false), trapRunning_(false), activeTasks_(0), trapSession_(nullptr) {
}

SnmpWorker::~SnmpWorker() {
    stopTrapReceiver();
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
            if (context->task.operation == SnmpOperation::SET) {
                if (context->task.setCallback) {
                    context->task.setCallback(true, "Success");
                }
            } else if (context->task.operation == SnmpOperation::INFORM) {
                if (context->task.informCallback) {
                    context->task.informCallback(true, "INFORM acknowledged");
                }
            } else {
                // GET / GETNEXT - build OID to variable list mapping
                std::vector<std::pair<std::string, netsnmp_variable_list*>> results;
                size_t idx = 0;
                for (netsnmp_variable_list* vars = pdu->variables;
                     vars != nullptr && idx < context->task.oids.size();
                     vars = vars->next_variable, ++idx) {
                    if (context->task.operation == SnmpOperation::GETNEXT) {
                        // GETNEXT: OID in response is different from what was requested
                        char oidBuf[256];
                        snprint_objid(oidBuf, sizeof(oidBuf), vars->name, vars->name_length);
                        results.push_back({std::string(oidBuf), vars});
                    } else {
                        // GET: response OID matches what was requested
                        results.push_back({context->task.oids[idx], vars});
                    }
                }
                if (context->task.callback) {
                    context->task.callback(results);
                }
            }
        } else {
            std::string errorMsg = "ERROR: " + std::string(snmp_errstring(pdu->errstat));
            if (context->task.operation == SnmpOperation::SET) {
                if (context->task.setCallback) {
                    context->task.setCallback(false, errorMsg);
                }
            } else if (context->task.operation == SnmpOperation::INFORM) {
                if (context->task.informCallback) {
                    context->task.informCallback(false, errorMsg);
                }
            } else {
                // GET operation error - pass empty vector
                if (context->task.callback) {
                    std::vector<std::pair<std::string, netsnmp_variable_list*>> emptyResults;
                    context->task.callback(emptyResults);
                }
            }
        }
    } else if (operation == NETSNMP_CALLBACK_OP_TIMED_OUT) {
        std::string errorMsg = "ERROR: Request timed out";
        if (context->task.operation == SnmpOperation::SET) {
            if (context->task.setCallback) {
                context->task.setCallback(false, errorMsg);
            }
        } else if (context->task.operation == SnmpOperation::INFORM) {
            if (context->task.informCallback) {
                context->task.informCallback(false, errorMsg);
            }
        } else {
            // GET operation timeout - pass empty vector
            if (context->task.callback) {
                std::vector<std::pair<std::string, netsnmp_variable_list*>> emptyResults;
                context->task.callback(emptyResults);
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
        if (task.operation == SnmpOperation::SET) {
            if (task.setCallback) {
                task.setCallback(false, errorMsg);
            }
        } else if (task.operation == SnmpOperation::INFORM) {
            if (task.informCallback) {
                task.informCallback(false, errorMsg);
            }
        } else {
            // GET operation - pass empty vector on error
            if (task.callback) {
                std::vector<std::pair<std::string, netsnmp_variable_list*>> emptyResults;
                task.callback(emptyResults);
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

    netsnmp_pdu* pdu = nullptr;

    if (task.operation == SnmpOperation::SET) {
        pdu = snmp_pdu_create(SNMP_MSG_SET);
        for (const auto& setValue : task.setValues) {
            oid oidArray[MAX_OID_LEN];
            size_t oidLen = MAX_OID_LEN;
            if (read_objid(setValue.oid.c_str(), oidArray, &oidLen)) {
                if (snmp_add_var(pdu, oidArray, oidLen, setValue.type, setValue.value.c_str()) != 0) {
                    std::cerr << "Failed to add SET variable: " << setValue.oid << std::endl;
                }
            }
        }
    } else if (task.operation == SnmpOperation::INFORM) {
        pdu = snmp_pdu_create(SNMP_MSG_INFORM);

        // Add sysUpTime.0 (required for INFORM)
        oid uptime_oid[MAX_OID_LEN];
        size_t uptime_oid_len = MAX_OID_LEN;
        if (read_objid("1.3.6.1.2.1.1.3.0", uptime_oid, &uptime_oid_len)) {
            long uptime = 0;  // Could be populated from actual system uptime
            snmp_pdu_add_variable(pdu, uptime_oid, uptime_oid_len, ASN_TIMETICKS, (u_char*)&uptime, sizeof(uptime));
        }

        // Add snmpTrapOID.0 (required for INFORM)
        oid trap_oid[MAX_OID_LEN];
        size_t trap_oid_len = MAX_OID_LEN;
        if (read_objid("1.3.6.1.6.3.1.1.4.1.0", trap_oid, &trap_oid_len)) {
            oid notification_oid[MAX_OID_LEN];
            size_t notification_oid_len = MAX_OID_LEN;
            if (read_objid(task.trapOid.c_str(), notification_oid, &notification_oid_len)) {
                snmp_pdu_add_variable(pdu, trap_oid, trap_oid_len, ASN_OBJECT_ID,
                                     (u_char*)notification_oid, notification_oid_len * sizeof(oid));
            }
        }

        // Add custom varbinds
        for (const auto& varbind : task.informVarbinds) {
            oid oidArray[MAX_OID_LEN];
            size_t oidLen = MAX_OID_LEN;
            if (read_objid(varbind.oid.c_str(), oidArray, &oidLen)) {
                if (snmp_add_var(pdu, oidArray, oidLen, varbind.type, varbind.value.c_str()) != 0) {
                    std::cerr << "Failed to add INFORM varbind: " << varbind.oid << std::endl;
                }
            }
        }
    } else {
        // GET or GETNEXT operation - always use oids vector
        pdu = snmp_pdu_create(task.operation == SnmpOperation::GETNEXT ? SNMP_MSG_GETNEXT : SNMP_MSG_GET);
        for (const auto& oidStr : task.oids) {
            oid oidArray[MAX_OID_LEN];
            size_t oidLen = MAX_OID_LEN;
            if (read_objid(oidStr.c_str(), oidArray, &oidLen)) {
                snmp_add_null_var(pdu, oidArray, oidLen);
            }
        }
    }

    if (snmp_send(sessionHandle, pdu) == 0) {
        snmp_free_pdu(pdu);
        std::string errorMsg = "ERROR: Failed to send request";
        if (task.operation == SnmpOperation::SET) {
            if (task.setCallback) {
                task.setCallback(false, errorMsg);
            }
        } else if (task.operation == SnmpOperation::INFORM) {
            if (task.informCallback) {
                task.informCallback(false, errorMsg);
            }
        } else {
            // GET operation - pass empty vector on error
            if (task.callback) {
                std::vector<std::pair<std::string, netsnmp_variable_list*>> emptyResults;
                task.callback(emptyResults);
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

                // Close the session and free the allocated strings (if they exist)
                snmp_close(cleanup.session);
                if (cleanup.peername) {
                    free(cleanup.peername);
                }
                if (cleanup.community) {
                    free(cleanup.community);
                }
            }
        }
    }
}

int SnmpWorker::trapCallback(int operation, netsnmp_session* session, int reqid, netsnmp_pdu* pdu, void* magic) {
    if (!magic || !pdu) {
        return 1;
    }

    SnmpWorker* worker = static_cast<SnmpWorker*>(magic);

    if (operation != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        return 1;
    }

    if (pdu->command != SNMP_MSG_TRAP && pdu->command != SNMP_MSG_TRAP2 && pdu->command != SNMP_MSG_INFORM) {
        return 1;
    }

    SnmpTrap trap;

    if (session && session->peername) {
        trap.sourceIp = session->peername;
    }

    if (session && session->community) {
        trap.community = std::string(reinterpret_cast<char*>(session->community), session->community_len);
    }

    if (pdu->command == SNMP_MSG_TRAP) {
        trap.enterpriseOid = "";
        if (pdu->enterprise) {
            char oidStr[MAX_OID_LEN * 4];
            snprint_objid(oidStr, sizeof(oidStr), pdu->enterprise, pdu->enterprise_length);
            trap.enterpriseOid = oidStr;
        }
        trap.genericTrap = pdu->trap_type;
        trap.specificTrap = pdu->specific_type;
        trap.uptime = pdu->time;
    } else {
        trap.enterpriseOid = "";
        trap.genericTrap = -1;
        trap.specificTrap = -1;
        trap.uptime = 0;
    }

    // Pass raw variable list pointer directly
    trap.varbinds = pdu->variables;

    if (worker->trapCallback_) {
        worker->trapCallback_(trap);
    }

    return 1;
}

void SnmpWorker::startTrapReceiver(int port, const std::function<void(const SnmpTrap&)>& callback) {
    std::lock_guard<std::mutex> trapLock(trapMutex_);

    if (trapRunning_.load()) {
        std::cerr << "Trap receiver already running" << std::endl;
        return;
    }

    netsnmp_session session;
    snmp_sess_init(&session);

    session.version = SNMP_VERSION_2c;
    session.peername = strdup("udp:162");
    session.callback = trapCallback;
    session.callback_magic = this;
    session.isAuthoritative = SNMP_SESS_UNKNOWNAUTH;

    SOCK_STARTUP;
    trapSession_ = snmp_open(&session);

    free(session.peername);

    if (!trapSession_) {
        std::cerr << "Failed to open trap receiver session" << std::endl;
        return;
    }

    trapCallback_ = callback;
    trapRunning_.store(true);
    std::cout << "SNMP Trap receiver started on port 162" << std::endl;
}

void SnmpWorker::stopTrapReceiver() {
    if (!trapRunning_.load()) {
        return;
    }

    trapRunning_.store(false);

    std::lock_guard<std::mutex> trapLock(trapMutex_);
    if (trapSession_) {
        // Queue the trap session for safe cleanup by the select thread
        std::lock_guard<std::mutex> sessionLock(sessionMutex_);
        SessionCleanup cleanup;
        cleanup.session = trapSession_;
        cleanup.peername = nullptr;  // peername was already freed in startTrapReceiver
        cleanup.community = nullptr;  // no community allocated for trap session
        sessionsToClose_.push(cleanup);

        trapSession_ = nullptr;
        std::cout << "SNMP Trap receiver stopped" << std::endl;
    }
}

