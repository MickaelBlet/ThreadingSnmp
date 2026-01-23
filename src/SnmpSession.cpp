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
    session_.peername = const_cast<char*>(host_.c_str());
    session_.community = reinterpret_cast<u_char*>(const_cast<char*>(community_.c_str()));
    session_.community_len = community_.length();
    session_.timeout = 1000000;
    session_.retries = 3;
}

bool SnmpSession::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isOpen_) {
        return true;
    }

    SOCK_STARTUP;
    init_snmp("ThreadingSnmp");

    sessionHandle_ = snmp_open(&session_);
    if (sessionHandle_ == nullptr) {
        char* err;
        snmp_error(&session_, nullptr, nullptr, &err);
        std::cerr << "Failed to open SNMP session: " << err << std::endl;
        free(err);
        return false;
    }

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

std::string SnmpSession::get(const std::string& oid) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpen_) {
        return "ERROR: Session not open";
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GET);
    oid oidArray[MAX_OID_LEN];
    size_t oidLen = MAX_OID_LEN;

    if (!read_objid(oid.c_str(), oidArray, &oidLen)) {
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

bool SnmpSession::set(const std::string& oid, char type, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!isOpen_) {
        return false;
    }

    netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_SET);
    oid oidArray[MAX_OID_LEN];
    size_t oidLen = MAX_OID_LEN;

    if (!read_objid(oid.c_str(), oidArray, &oidLen)) {
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
