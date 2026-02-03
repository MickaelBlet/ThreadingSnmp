#ifndef SNMP_SESSION_H
#define SNMP_SESSION_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

class SnmpSession {
public:
    SnmpSession(const std::string& host, const std::string& community = "public", int version = SNMP_VERSION_2c);
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
    bool set(const std::string& oid, char type, const std::string& value);

private:
    std::string host_;
    std::string community_;
    int version_;
    netsnmp_session session_;
    netsnmp_session* sessionHandle_;
    mutable std::mutex mutex_;
    bool isOpen_;

    void initializeSession();
};

#endif
