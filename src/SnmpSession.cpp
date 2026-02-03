#include "SnmpSession.h"
#include <iostream>
#include <cstring>

SnmpSession::SnmpSession(const std::string& host, const std::string& community, int version)
    : host_(host), community_(community), version_(version), sessionHandle_(nullptr), isOpen_(false) {
    initializeSession();
}

SnmpSession::~SnmpSession() {
    close();
}

void SnmpSession::initializeSession() {
    snmp_sess_init(&session_);
    session_.version = version_;
    // Note: snmp_open() will take ownership of these allocated strings
    // and free them when snmp_close() is called
    session_.peername = strdup(host_.c_str());
    session_.community = reinterpret_cast<u_char*>(strdup(community_.c_str()));
    session_.community_len = community_.length();
    session_.timeout = 1000000;
    session_.retries = 3;
}

bool SnmpSession::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isOpen_) {
        return true;
    }

    sessionHandle_ = snmp_open(&session_);
    if (sessionHandle_ == nullptr) {
        char* err;
        snmp_error(&session_, nullptr, nullptr, &err);
        std::cerr << "Failed to open SNMP session: " << err << std::endl;
        free(err);
        // snmp_open failed, so we need to free the allocated strings ourselves
        free(session_.peername);
        free(session_.community);
        session_.peername = nullptr;
        session_.community = nullptr;
        return false;
    }

    // snmp_open() succeeded and made copies of peername and community.
    // Free our originals to avoid memory leaks.
    free(session_.peername);
    free(session_.community);
    session_.peername = nullptr;
    session_.community = nullptr;

    isOpen_ = true;
    return true;
}

void SnmpSession::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (sessionHandle_ != nullptr) {
        snmp_close(sessionHandle_);
        sessionHandle_ = nullptr;
    }

    isOpen_ = false;
}

bool SnmpSession::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isOpen_;
}

std::string SnmpSession::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpen_) {
        return "ERROR: Session not open";
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GET);
    oid oidArray[MAX_OID_LEN];
    size_t oidLen = MAX_OID_LEN;

    if (!read_objid(id.c_str(), oidArray, &oidLen)) {
        snmp_free_pdu(pdu);
        return "ERROR: Invalid OID";
    }

    snmp_add_null_var(pdu, oidArray, oidLen);

    netsnmp_pdu* response = nullptr;
    int status = snmp_synch_response(sessionHandle_, pdu, &response);

    std::string result;
    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        for (netsnmp_variable_list* vars = response->variables; vars != nullptr; vars = vars->next_variable) {
            char buf[1024];
            snprint_value(buf, sizeof(buf), vars->name, vars->name_length, vars);
            result = buf;
        }
    } else {
        if (status == STAT_SUCCESS) {
            result = "ERROR: " + std::string(snmp_errstring(response->errstat));
        } else {
            char* err;
            snmp_error(sessionHandle_, nullptr, nullptr, &err);
            result = "ERROR: " + std::string(err);
            free(err);
        }
    }

    if (response != nullptr) {
        snmp_free_pdu(response);
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> SnmpSession::getMulti(const std::vector<std::string>& oids) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::pair<std::string, std::string>> results;

    if (!isOpen_) {
        for (const auto& oid : oids) {
            results.push_back({oid, "ERROR: Session not open"});
        }
        return results;
    }

    if (oids.empty()) {
        return results;
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GET);

    for (const auto& oidStr : oids) {
        oid oidArray[MAX_OID_LEN];
        size_t oidLen = MAX_OID_LEN;

        if (!read_objid(oidStr.c_str(), oidArray, &oidLen)) {
            snmp_free_pdu(pdu);
            for (const auto& o : oids) {
                results.push_back({o, "ERROR: Invalid OID"});
            }
            return results;
        }

        snmp_add_null_var(pdu, oidArray, oidLen);
    }

    netsnmp_pdu* response = nullptr;
    int status = snmp_synch_response(sessionHandle_, pdu, &response);

    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        size_t idx = 0;
        for (netsnmp_variable_list* vars = response->variables; vars != nullptr && idx < oids.size(); vars = vars->next_variable, ++idx) {
            char buf[1024];
            snprint_value(buf, sizeof(buf), vars->name, vars->name_length, vars);
            results.push_back({oids[idx], std::string(buf)});
        }
    } else {
        std::string errorMsg;
        if (status == STAT_SUCCESS) {
            errorMsg = "ERROR: " + std::string(snmp_errstring(response->errstat));
        } else {
            char* err;
            snmp_error(sessionHandle_, nullptr, nullptr, &err);
            errorMsg = "ERROR: " + std::string(err);
            free(err);
        }
        for (const auto& oidStr : oids) {
            results.push_back({oidStr, errorMsg});
        }
    }

    if (response != nullptr) {
        snmp_free_pdu(response);
    }

    return results;
}

std::pair<std::string, std::string> SnmpSession::getNext(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpen_) {
        return {"", "ERROR: Session not open"};
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
    oid oidArray[MAX_OID_LEN];
    size_t oidLen = MAX_OID_LEN;

    if (!read_objid(id.c_str(), oidArray, &oidLen)) {
        snmp_free_pdu(pdu);
        return {"", "ERROR: Invalid OID"};
    }

    snmp_add_null_var(pdu, oidArray, oidLen);

    netsnmp_pdu* response = nullptr;
    int status = snmp_synch_response(sessionHandle_, pdu, &response);

    std::pair<std::string, std::string> result;
    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        if (response->variables) {
            char oidBuf[256];
            char valBuf[1024];
            snprint_objid(oidBuf, sizeof(oidBuf), response->variables->name, response->variables->name_length);
            snprint_value(valBuf, sizeof(valBuf), response->variables->name, response->variables->name_length, response->variables);
            result = {std::string(oidBuf), std::string(valBuf)};
        }
    } else {
        if (status == STAT_SUCCESS) {
            result = {"", "ERROR: " + std::string(snmp_errstring(response->errstat))};
        } else {
            char* err;
            snmp_error(sessionHandle_, nullptr, nullptr, &err);
            result = {"", "ERROR: " + std::string(err)};
            free(err);
        }
    }

    if (response != nullptr) {
        snmp_free_pdu(response);
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> SnmpSession::getNextMulti(const std::vector<std::string>& oids) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::pair<std::string, std::string>> results;

    if (!isOpen_) {
        for (size_t i = 0; i < oids.size(); i++) {
            results.push_back({"", "ERROR: Session not open"});
        }
        return results;
    }

    if (oids.empty()) {
        return results;
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);

    for (const auto& oidStr : oids) {
        oid oidArray[MAX_OID_LEN];
        size_t oidLen = MAX_OID_LEN;

        if (!read_objid(oidStr.c_str(), oidArray, &oidLen)) {
            snmp_free_pdu(pdu);
            for (size_t i = 0; i < oids.size(); i++) {
                results.push_back({"", "ERROR: Invalid OID"});
            }
            return results;
        }

        snmp_add_null_var(pdu, oidArray, oidLen);
    }

    netsnmp_pdu* response = nullptr;
    int status = snmp_synch_response(sessionHandle_, pdu, &response);

    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        for (netsnmp_variable_list* vars = response->variables; vars != nullptr; vars = vars->next_variable) {
            char oidBuf[256];
            char valBuf[1024];
            snprint_objid(oidBuf, sizeof(oidBuf), vars->name, vars->name_length);
            snprint_value(valBuf, sizeof(valBuf), vars->name, vars->name_length, vars);
            results.push_back({std::string(oidBuf), std::string(valBuf)});
        }
    } else {
        std::string errorMsg;
        if (status == STAT_SUCCESS) {
            errorMsg = "ERROR: " + std::string(snmp_errstring(response->errstat));
        } else {
            char* err;
            snmp_error(sessionHandle_, nullptr, nullptr, &err);
            errorMsg = "ERROR: " + std::string(err);
            free(err);
        }
        for (size_t i = 0; i < oids.size(); i++) {
            results.push_back({"", errorMsg});
        }
    }

    if (response != nullptr) {
        snmp_free_pdu(response);
    }

    return results;
}

bool SnmpSession::set(const std::string& id, char type, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpen_) {
        return false;
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_SET);
    oid oidArray[MAX_OID_LEN];
    size_t oidLen = MAX_OID_LEN;

    if (!read_objid(id.c_str(), oidArray, &oidLen)) {
        snmp_free_pdu(pdu);
        return false;
    }

    if (snmp_add_var(pdu, oidArray, oidLen, type, value.c_str()) != 0) {
        snmp_free_pdu(pdu);
        return false;
    }

    netsnmp_pdu* response = nullptr;
    int status = snmp_synch_response(sessionHandle_, pdu, &response);

    bool success = (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR);

    if (response != nullptr) {
        snmp_free_pdu(response);
    }

    return success;
}
