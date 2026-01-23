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
- Configurable thread pool for concurrent queries
- Support for SNMPv1, SNMPv2c, and SNMPv3
- Asynchronous task processing with callbacks
- Clean RAII-based resource management

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

#### Single SNMP Session

```cpp
#include "SnmpSession.h"

SnmpSession session("localhost", "public");

if (session.open()) {
    std::string result = session.get("1.3.6.1.2.1.1.1.0");
    std::cout << "System Description: " << result << std::endl;
    session.close();
}
```

#### Multi-threaded SNMP Queries

```cpp
#include "SnmpWorker.h"

SnmpWorker worker(4);  // 4 worker threads
worker.start();

SnmpTask task;
task.host = "localhost";
task.community = "public";
task.oid = "1.3.6.1.2.1.1.1.0";
task.callback = [](const std::string& oid, const std::string& result) {
    std::cout << "OID: " << oid << " -> " << result << std::endl;
};

worker.addTask(task);
worker.wait();  // Wait for all tasks to complete
worker.stop();
```

## Architecture

### SnmpSession Class

Manages individual SNMP sessions with thread-safety:
- Mutex-protected session operations
- Support for GET and SET operations
- Automatic resource cleanup via RAII
- Error handling with descriptive messages

### SnmpWorker Class

Provides a thread pool for concurrent SNMP operations:
- Configurable number of worker threads
- Task queue with condition variables
- Callback-based result handling
- Graceful shutdown and cleanup

## Common SNMP OIDs

| OID | Description |
|-----|-------------|
| 1.3.6.1.2.1.1.1.0 | System Description |
| 1.3.6.1.2.1.1.2.0 | System Object ID |
| 1.3.6.1.2.1.1.3.0 | System Uptime |
| 1.3.6.1.2.1.1.4.0 | System Contact |
| 1.3.6.1.2.1.1.5.0 | System Name |
| 1.3.6.1.2.1.1.6.0 | System Location |

## Thread Safety Considerations

When using net-snmp in a multi-threaded environment:

1. **Session Isolation**: Each thread should use its own `SnmpSession` instance
2. **Mutex Protection**: All session operations are protected by mutexes
3. **Proper Initialization**: `init_snmp()` is called once per session
4. **Resource Cleanup**: Sessions are automatically closed in destructors

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
