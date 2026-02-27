#ifndef SNMP_SESSION_H
#define SNMP_SESSION_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

struct SnmpV3Config {
    std::string securityName;         // Username
    int securityLevel;                // SNMP_SEC_LEVEL_NOAUTH, SNMP_SEC_LEVEL_AUTHNOPRIV, SNMP_SEC_LEVEL_AUTHPRIV
    oid* authProtocol;                // usmHMACMD5AuthProtocol, usmHMACSHA1AuthProtocol
    size_t authProtocolLen;
    std::string authPassword;
    oid* privProtocol;                // usmDESPrivProtocol, usmAESPrivProtocol
    size_t privProtocolLen;
    std::string privPassword;
    std::string contextName;          // Optional
    std::string contextEngineID;      // Optional

    SnmpV3Config() : securityLevel(SNMP_SEC_LEVEL_NOAUTH),
                     authProtocol(nullptr), authProtocolLen(0),
                     privProtocol(nullptr), privProtocolLen(0) {}
};

class SnmpSession {
public:
    SnmpSession(const std::string& host, const std::string& community = "public", int version = SNMP_VERSION_2c);
    SnmpSession(const std::string& host, const SnmpV3Config& v3config);  // SNMPv3 constructor
    ~SnmpSession();

    SnmpSession(const SnmpSession&) = delete;
    SnmpSession& operator=(const SnmpSession&) = delete;

    bool open();
    void close();
    bool isOpen() const;

    std::string get(const std::string& oid);
    std::vector<std::pair<std::string, std::string>> getMulti(const std::vector<std::string>& oids);
    std::pair<std::string, std::string> getNext(const std::string& oid);
    std::vector<std::pair<std::string, std::string>> getNextMulti(const std::vector<std::string>& oids);
    std::vector<std::pair<std::string, std::string>> walk(const std::string& baseOid);
    bool set(const std::string& oid, char type, const std::string& value);

private:
    std::string host_;
    std::string community_;
    int version_;
    SnmpV3Config v3config_;
    netsnmp_session session_;
    netsnmp_session* sessionHandle_;
    mutable std::mutex mutex_;
    bool isOpen_;

    void initializeSession();
};

#endif
