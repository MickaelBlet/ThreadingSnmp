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

// Single OID per task
SnmpTask task;
task.host = "localhost";
task.community = "public";
task.oid = "1.3.6.1.2.1.1.1.0";
task.isMultiOid = false;
task.callback = [](const std::string& oid, const std::string& result) {
    std::cout << "OID: " << oid << " -> " << result << std::endl;
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

SnmpTask task;
task.host = "localhost";
task.community = "public";
task.oids = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.1.5.0"};
task.isMultiOid = true;
task.multiCallback = [](const std::vector<std::pair<std::string, std::string>>& results) {
    for (const auto& result : results) {
        std::cout << result.first << " -> " << result.second << std::endl;
    }
};

worker.addTask(task);
worker.wait();
worker.stop();
```

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
- Support for both single-OID and multi-OID async requests
- Task queue with condition variables
- Callback-based result handling (single callback or multi-callback)
- Graceful shutdown and cleanup
- Better resource usage compared to thread-per-request approach

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
4. `snmp_select_info()` and `select()` monitor all active sessions
5. When data arrives, `snmp_read()` processes responses
6. Callbacks are invoked with results
7. Sessions are cleaned up automatically

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
