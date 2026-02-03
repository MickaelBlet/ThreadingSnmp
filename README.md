# ThreadingSnmp

A C++14 project demonstrating thread-safe usage of the net-snmp library for SNMP operations.

## Overview

This project provides a thread-safe wrapper around the net-snmp library, enabling concurrent SNMP queries across multiple hosts and OIDs. It includes:

- **SnmpSession**: Thread-safe SNMP session management with mutex protection
- **SnmpWorker**: Thread pool for executing SNMP queries concurrently
- Complete example demonstrating single and multi-threaded SNMP operations

## Features

- C++14 standard compliance
- Thread-safe SNMP session handling
- **Multi-OID GET support**: Query multiple OIDs in a single SNMP request
- **Async SET operations**: Modify SNMP values asynchronously
- **Async INFORM operations**: Send acknowledged SNMP notifications
- **SNMP Trap Receiver**: Built-in trap/inform listener for monitoring notifications
- **snmp_select based architecture**: Efficient async I/O using a single select thread
- Support for SNMPv1, SNMPv2c, and SNMPv3
- Asynchronous task processing with callbacks
- Clean RAII-based resource management
- Better performance compared to traditional multi-threaded blocking approach

## Requirements

### Dependencies

- C++14 compatible compiler (GCC 5+, Clang 3.4+, MSVC 2015+)
- CMake 3.10 or higher
- net-snmp library and development headers
- pthread library (usually included with GCC)
- OpenSSL library (for crypto support)

### Installing net-snmp

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install libsnmp-dev snmp
```

**CentOS/RHEL/Fedora:**
```bash
sudo yum install net-snmp-devel net-snmp
```

**macOS:**
```bash
brew install net-snmp
```

## Building

```bash
mkdir build
cd build
cmake ..
make
```

This will create:
- `libthreading_snmp.a`: Static library for SNMP threading
- `snmp_thread_example`: Example executable

## Usage

### Running the Example

```bash
./snmp_thread_example <host> [community]
```

Example:
```bash
./snmp_thread_example localhost public
./snmp_thread_example 192.168.1.1 public
```

### Using in Your Code

#### Single SNMP Session - Single OID GET

```cpp
#include "SnmpSession.h"

SnmpSession session("localhost", "public");

if (session.open()) {
    std::string result = session.get("1.3.6.1.2.1.1.1.0");
    std::cout << "System Description: " << result << std::endl;
    session.close();
}
```

#### Single SNMP Session - Multi-OID GET

```cpp
#include "SnmpSession.h"

SnmpSession session("localhost", "public");

if (session.open()) {
    std::vector<std::string> oids = {
        "1.3.6.1.2.1.1.1.0",  // sysDescr
        "1.3.6.1.2.1.1.3.0",  // sysUpTime
        "1.3.6.1.2.1.1.5.0"   // sysName
    };

    auto results = session.getMulti(oids);
    for (const auto& result : results) {
        std::cout << result.first << " -> " << result.second << std::endl;
    }
    session.close();
}
```

#### Async SNMP with snmp_select

```cpp
#include "SnmpWorker.h"

SnmpWorker worker;  // Single select thread
worker.start();

// Single OID per task (using vector)
SnmpTask task;
task.host = "localhost";
task.community = "public";
task.oids = {"1.3.6.1.2.1.1.1.0"};  // Single OID in vector
task.callback = [](const std::vector<std::pair<std::string, netsnmp_variable_list*>>& results) {
    if (results.empty()) {
        std::cout << "ERROR: Request failed" << std::endl;
        return;
    }
    // Iterate through OID-variable pairs for direct access to SNMP data
    for (const auto& [oid, var] : results) {
        char valBuf[1024];
        snprint_value(valBuf, sizeof(valBuf), var->name, var->name_length, var);
        std::cout << oid << " -> " << valBuf << std::endl;
    }
};

worker.addTask(task);
worker.wait();  // Wait for all tasks to complete
worker.stop();
```

#### Async Multi-OID GET with snmp_select

```cpp
#include "SnmpWorker.h"

SnmpWorker worker;
worker.start();

// Multiple OIDs per task (same unified API)
SnmpTask task;
task.host = "localhost";
task.community = "public";
task.oids = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.1.5.0"};
task.callback = [](const std::vector<std::pair<std::string, netsnmp_variable_list*>>& results) {
    if (results.empty()) {
        std::cout << "ERROR: Request failed" << std::endl;
        return;
    }
    // Direct access to netsnmp_variable_list via OID-variable pairs
    for (const auto& [oid, var] : results) {
        char valBuf[1024];
        snprint_value(valBuf, sizeof(valBuf), var->name, var->name_length, var);
        std::cout << oid << " -> " << valBuf << std::endl;
    }
};

worker.addTask(task);
worker.wait();
worker.stop();
```

#### Async SNMP SET Operation

```cpp
#include "SnmpWorker.h"

SnmpWorker worker;
worker.start();

SnmpTask setTask;
setTask.host = "localhost";
setTask.community = "private";
setTask.operation = SnmpOperation::SET;
setTask.setValues = {
    {"1.3.6.1.2.1.1.4.0", 's', "admin@example.com"},  // sysContact
    {"1.3.6.1.2.1.1.6.0", 's', "Server Room A"}       // sysLocation
};
setTask.setCallback = [](bool success, const std::string& message) {
    if (success) {
        std::cout << "SET succeeded: " << message << std::endl;
    } else {
        std::cout << "SET failed: " << message << std::endl;
    }
};

worker.addTask(setTask);
worker.wait();
worker.stop();
```

**Common SNMP Type Codes for SET:**
- `'i'` - Integer
- `'s'` - String
- `'x'` - Hex String
- `'d'` - Decimal String
- `'n'` - Null
- `'o'` - Object ID
- `'t'` - Time Ticks
- `'a'` - IP Address
- `'u'` - Unsigned Integer

#### SNMP Trap Receiver

The trap receiver can run concurrently with GET/SET/INFORM operations - they all share the same select thread for efficient I/O multiplexing.

```cpp
#include "SnmpWorker.h"

SnmpWorker worker;
worker.start();

// Define trap handler callback with raw variable list access
auto trapHandler = [](const SnmpTrap& trap) {
    std::cout << "Trap received from: " << trap.sourceIp << std::endl;
    std::cout << "Community: " << trap.community << std::endl;

    // Direct access to varbinds via netsnmp_variable_list*
    for (netsnmp_variable_list* v = trap.varbinds; v != nullptr; v = v->next_variable) {
        char oidBuf[256];
        char valBuf[1024];
        snprint_objid(oidBuf, sizeof(oidBuf), v->name, v->name_length);
        snprint_value(valBuf, sizeof(valBuf), v->name, v->name_length, v);
        std::cout << "  " << oidBuf << " = " << valBuf << std::endl;
    }
};

// Start trap receiver on port 162 (default SNMP trap port)
worker.startTrapReceiver(162, trapHandler);

// You can perform GET/SET operations while trap receiver is active!
SnmpTask task;
task.host = "192.168.1.1";
task.community = "public";
task.oids = {"1.3.6.1.2.1.1.1.0"};
task.callback = [](const std::vector<std::pair<std::string, netsnmp_variable_list*>>& results) {
    // Process OID-variable pairs...
};
worker.addTask(task);  // This works concurrently with trap receiver

worker.wait();  // Wait for GET operations to complete

// Trap receiver continues running in background...

// Stop trap receiver when done
worker.stopTrapReceiver();
worker.stop();
```

**Testing Trap Receiver:**
```bash
# Send a test trap using snmptrap command
snmptrap -v 2c -c public localhost '' 1.3.6.1.4.1.8072.2.3.0.1 \
    1.3.6.1.4.1.8072.2.3.2.1 i 123456
```

#### SNMP INFORM Operation

```cpp
#include "SnmpWorker.h"

SnmpWorker worker;
worker.start();

SnmpTask informTask;
informTask.host = "localhost";
informTask.community = "public";
informTask.operation = SnmpOperation::INFORM;
informTask.trapOid = "1.3.6.1.4.1.8072.2.3.0.1";  // Notification OID

// Add custom varbinds (optional)
informTask.informVarbinds = {
    {"1.3.6.1.4.1.8072.2.3.2.1", 'i', "12345"},       // Integer
    {"1.3.6.1.4.1.8072.2.3.2.2", 's', "Test Inform"}  // String
};

informTask.informCallback = [](bool success, const std::string& message) {
    if (success) {
        std::cout << "INFORM acknowledged: " << message << std::endl;
    } else {
        std::cout << "INFORM failed: " << message << std::endl;
    }
};

worker.addTask(informTask);
worker.wait();
worker.stop();
```

**INFORM vs TRAP:**
- **INFORM**: Acknowledged notification - manager must send response
- **TRAP**: Fire-and-forget notification - no acknowledgment required
- INFORMs are more reliable but require more network overhead
- Use INFORM when you need confirmation that notification was received
- Use TRAP for high-volume, best-effort notifications

**Required OIDs for INFORM:**
- `1.3.6.1.2.1.1.3.0` - sysUpTime.0 (automatically added)
- `1.3.6.1.6.3.1.1.4.1.0` - snmpTrapOID.0 (automatically added)
- Custom varbinds can be added via `informVarbinds`

#### Multiple Workers (Concurrent Operation Types)

You can run multiple SnmpWorker instances concurrently - each with its own select thread. This is useful when you want to dedicate workers to specific operation types (e.g., one for INFORM, one for GET).

```cpp
#include "SnmpWorker.h"

// Worker 1: Dedicated to INFORM operations
SnmpWorker informWorker;
informWorker.start();

// Worker 2: Dedicated to GET operations
SnmpWorker getWorker;
getWorker.start();

// Launch INFORM tasks on worker 1
SnmpTask informTask;
informTask.operation = SnmpOperation::INFORM;
informTask.host = "localhost";
informTask.community = "public";
informTask.trapOid = "1.3.6.1.4.1.8072.2.3.0.1";
informTask.informCallback = [](bool success, const std::string& msg) {
    std::cout << "INFORM: " << msg << std::endl;
};
informWorker.addTask(informTask);

// Launch GET tasks on worker 2 (runs concurrently!)
SnmpTask getTask;
getTask.host = "localhost";
getTask.community = "public";
getTask.oids = {"1.3.6.1.2.1.1.1.0"};
getTask.callback = [](const auto& results) {
    // Process results...
};
getWorker.addTask(getTask);

// Wait for both workers
informWorker.wait();
getWorker.wait();

// Stop both workers
informWorker.stop();
getWorker.stop();
```

**Key Points:**
- Each SnmpWorker has its own select thread and manages its own sessions
- `snmp_select_info()` is thread-safe and can be called from multiple threads
- Workers are completely independent and can run different operation types concurrently
- See `examples/multi_worker_example.cpp` for a complete demonstration

## Architecture

### SnmpSession Class

Manages individual SNMP sessions with thread-safety:
- Mutex-protected session operations
- Support for single OID GET operations
- **Support for multi-OID GET operations** - retrieve multiple OIDs in a single request
- Support for SET operations
- Automatic resource cleanup via RAII
- Error handling with descriptive messages

### SnmpWorker Class

Provides asynchronous SNMP operations using **snmp_select**:
- **Single select thread** instead of multiple worker threads
- Uses `snmp_select()` for efficient async I/O multiplexing
- Asynchronous SNMP requests with `snmp_send()` and callbacks
- **Unified API** - always use vector for OIDs (single or multiple), single callback type
- Support for async GET requests with single or multiple OIDs
- **Support for async SET operations** - modify SNMP values asynchronously
- **Support for async INFORM operations** - send acknowledged notifications
- **Built-in SNMP trap receiver** - listen for trap/inform notifications
- Task queue with condition variables
- Callback-based result handling (GET, SET, INFORM, and trap callbacks)
- **Unified thread architecture** - single thread handles both outgoing operations and incoming traps
- Graceful shutdown and cleanup
- Better resource usage compared to thread-per-request approach

### SnmpTrap Structure

Contains trap notification information:
- `sourceIp` - IP address of the trap sender
- `community` - Community string used
- `enterpriseOid` - Enterprise OID (for SNMPv1 traps)
- `genericTrap` - Generic trap type
- `specificTrap` - Specific trap type
- `uptime` - System uptime when trap was generated
- `varbinds` - Vector of OID-value pairs containing trap data

## Common SNMP OIDs

| OID | Description |
|-----|-------------|
| 1.3.6.1.2.1.1.1.0 | System Description |
| 1.3.6.1.2.1.1.2.0 | System Object ID |
| 1.3.6.1.2.1.1.3.0 | System Uptime |
| 1.3.6.1.2.1.1.4.0 | System Contact |
| 1.3.6.1.2.1.1.5.0 | System Name |
| 1.3.6.1.2.1.1.6.0 | System Location |

## Why snmp_select?

The traditional multi-threaded approach creates one thread per SNMP request, which can be inefficient:
- High overhead with many concurrent requests
- Thread context switching costs
- Resource consumption (stack memory per thread)

The **snmp_select** approach used in this library is more efficient:
- **Single thread** handles all SNMP I/O using `select()`
- Asynchronous non-blocking operations
- Lower memory footprint
- Better scalability for many concurrent requests
- Native support in net-snmp library

### How It Works

1. Tasks are queued by the application
2. Single select thread processes the queue
3. For each task, an async SNMP request is sent using `snmp_send()`
4. `snmp_select_info()` and `select()` monitor all active sessions (including trap receiver if enabled)
5. When data arrives, `snmp_read()` processes responses:
   - Outgoing operations (GET/SET/INFORM) trigger asyncCallback
   - Incoming traps/informs trigger trapCallback
6. Callbacks are invoked with results
7. Sessions are cleaned up automatically

**Unified Thread Architecture:**
- One thread handles both outgoing SNMP operations and incoming trap notifications
- The trap receiver session is added to the same select() monitoring as task sessions
- Eliminates the need for a separate trap receiver thread
- More efficient resource usage and simpler architecture

## Thread Safety Considerations

When using net-snmp in this library:

1. **Session Isolation**: Each SNMP request uses its own session instance
2. **Mutex Protection**: All shared data structures are protected by mutexes
3. **Proper Initialization**: `init_snmp()` is called once during SnmpWorker construction
4. **Resource Cleanup**: Sessions are automatically closed after callbacks complete
5. **Single Select Thread**: All SNMP I/O is handled by one thread using `select()`

## Troubleshooting

### Build Issues

**net-snmp not found:**
```
CMake Error: net-snmp library not found
```
Solution: Install libsnmp-dev (see Requirements section)

**Linking errors:**
Ensure you have both the runtime and development packages installed.

### Runtime Issues

**Connection timeout:**
- Verify the target host is reachable
- Check SNMP service is running on target
- Verify community string is correct
- Check firewall rules (UDP port 161)

**Permission denied:**
Some SNMP OIDs require specific access rights. Verify your community string has appropriate permissions.

## Project Structure

```
ThreadingSnmp/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── include/
│   ├── SnmpSession.h       # SNMP session wrapper
│   └── SnmpWorker.h        # Thread pool worker
├── src/
│   ├── SnmpSession.cpp     # Session implementation
│   └── SnmpWorker.cpp      # Worker implementation
└── examples/
    └── main.cpp            # Example usage
```

## License

See LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues.

## References

- [Net-SNMP Documentation](http://www.net-snmp.org/docs/)
- [SNMP RFCs](https://www.ietf.org/rfc/)
- [CMake Documentation](https://cmake.org/documentation/)
