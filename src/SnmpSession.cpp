#include "SnmpSession.h"
#include <iostream>
#include <cstring>

SnmpSession::SnmpSession(const std::string& host, const std::string& community, int version)
    : host_(host), community_(community), version_(version), sessionHandle_(nullptr), isOpen_(false) {
    initializeSession();
}

SnmpSession::SnmpSession(const std::string& host, const SnmpV3Config& v3config)
    : host_(host), version_(SNMP_VERSION_3), v3config_(v3config), sessionHandle_(nullptr), isOpen_(false) {
    initializeSession();
}

SnmpSession::~SnmpSession() {
    close();
}

void SnmpSession::initializeSession() {
    snmp_sess_init(&session_);
    session_.version = version_;
    session_.peername = strdup(host_.c_str());
    session_.timeout = 1000000;
    session_.retries = 3;

    if (version_ == SNMP_VERSION_3) {
        // SNMPv3 configuration
        session_.securityName = strdup(v3config_.securityName.c_str());
        session_.securityNameLen = v3config_.securityName.length();
        session_.securityLevel = v3config_.securityLevel;

        // Authentication setup
        if (v3config_.securityLevel >= SNMP_SEC_LEVEL_AUTHNOPRIV) {
            session_.securityAuthProto = v3config_.authProtocol;
            session_.securityAuthProtoLen = v3config_.authProtocolLen;

            if (!v3config_.authPassword.empty()) {
                session_.securityAuthKeyLen = USM_AUTH_KU_LEN;
                if (generate_Ku(session_.securityAuthProto, session_.securityAuthProtoLen,
                                reinterpret_cast<const u_char*>(v3config_.authPassword.c_str()),
                                v3config_.authPassword.length(),
                                session_.securityAuthKey, &session_.securityAuthKeyLen) != SNMPERR_SUCCESS) {
                    std::cerr << "Error generating authentication key" << std::endl;
                }
            }
        }

        // Privacy setup
        if (v3config_.securityLevel >= SNMP_SEC_LEVEL_AUTHPRIV) {
            session_.securityPrivProto = v3config_.privProtocol;
            session_.securityPrivProtoLen = v3config_.privProtocolLen;

            if (!v3config_.privPassword.empty()) {
                session_.securityPrivKeyLen = USM_PRIV_KU_LEN;
                if (generate_Ku(session_.securityAuthProto, session_.securityAuthProtoLen,
                                reinterpret_cast<const u_char*>(v3config_.privPassword.c_str()),
                                v3config_.privPassword.length(),
                                session_.securityPrivKey, &session_.securityPrivKeyLen) != SNMPERR_SUCCESS) {
                    std::cerr << "Error generating privacy key" << std::endl;
                }
            }
        }

        // Optional context
        if (!v3config_.contextName.empty()) {
            session_.contextName = strdup(v3config_.contextName.c_str());
            session_.contextNameLen = v3config_.contextName.length();
        }
    } else {
        // SNMPv1/v2c configuration
        session_.community = reinterpret_cast<u_char*>(strdup(community_.c_str()));
        session_.community_len = community_.length();
    }
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
        if (version_ == SNMP_VERSION_3) {
            free(session_.securityName);
            if (session_.contextName) free(session_.contextName);
        } else {
            free(session_.community);
        }
        session_.peername = nullptr;
        session_.community = nullptr;
        session_.securityName = nullptr;
        session_.contextName = nullptr;
        return false;
    }

    // snmp_open() succeeded and made copies of allocated strings.
    // Free our originals to avoid memory leaks.
    free(session_.peername);
    if (version_ == SNMP_VERSION_3) {
        free(session_.securityName);
        if (session_.contextName) free(session_.contextName);
        session_.securityName = nullptr;
        session_.contextName = nullptr;
    } else {
        free(session_.community);
        session_.community = nullptr;
    }
    session_.peername = nullptr;

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

std::vector<std::pair<std::string, std::string>> SnmpSession::walk(const std::string& baseOid) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::pair<std::string, std::string>> results;

    if (!isOpen_) {
        return results;
    }

    oid baseOidArray[MAX_OID_LEN];
    size_t baseOidLen = MAX_OID_LEN;
    if (!read_objid(baseOid.c_str(), baseOidArray, &baseOidLen)) {
        return results;
    }

    oid currentOid[MAX_OID_LEN];
    size_t currentOidLen = baseOidLen;
    memcpy(currentOid, baseOidArray, baseOidLen * sizeof(oid));

    while (true) {
        netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
        snmp_add_null_var(pdu, currentOid, currentOidLen);

        netsnmp_pdu* response = nullptr;
        int status = snmp_synch_response(sessionHandle_, pdu, &response);

        if (status != STAT_SUCCESS || !response || response->errstat != SNMP_ERR_NOERROR) {
            if (response) snmp_free_pdu(response);
            break;
        }

        netsnmp_variable_list* vars = response->variables;
        if (!vars || vars->type == SNMP_ENDOFMIBVIEW) {
            snmp_free_pdu(response);
            break;
        }

        // Check if response OID is still within the base subtree
        bool inSubtree = (vars->name_length > baseOidLen);
        if (inSubtree) {
            for (size_t i = 0; i < baseOidLen; i++) {
                if (vars->name[i] != baseOidArray[i]) {
                    inSubtree = false;
                    break;
                }
            }
        }

        if (!inSubtree) {
            snmp_free_pdu(response);
            break;
        }

        char oidBuf[256];
        char valBuf[1024];
        snprint_objid(oidBuf, sizeof(oidBuf), vars->name, vars->name_length);
        snprint_value(valBuf, sizeof(valBuf), vars->name, vars->name_length, vars);
        results.push_back({std::string(oidBuf), std::string(valBuf)});

        // Advance to the response OID for next iteration
        currentOidLen = vars->name_length;
        memcpy(currentOid, vars->name, vars->name_length * sizeof(oid));

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
