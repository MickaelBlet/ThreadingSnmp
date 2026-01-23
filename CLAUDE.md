# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ThreadingSnmp is a C++14 thread-safe wrapper around the net-snmp library for concurrent SNMP operations. The project uses a **single-threaded select-based architecture** for efficient asynchronous I/O, rather than traditional multi-threaded blocking approaches.

## Build Commands

```bash
# Build the project
mkdir build
cd build
cmake ..
make

# Run the example
./snmp_thread_example <host> [community]
./snmp_thread_example localhost public
```

The build produces:
- `libthreading_snmp.a` - Static library
- `snmp_thread_example` - Example executable demonstrating all usage patterns

## Dependencies

- C++14 compiler (GCC 5+, Clang 3.4+)
- CMake 3.10+
- net-snmp library (`libsnmp-dev` on Debian/Ubuntu, `net-snmp-devel` on RHEL/CentOS)
- pthread
- OpenSSL (crypto)

## Architecture

### Core Design Pattern: snmp_select-based Async I/O

The architecture uses **a single select thread** instead of thread-per-request to achieve better scalability:

1. **SnmpWorker** maintains a single select thread that processes a task queue
2. Tasks are queued with `addTask()` and processed asynchronously
3. The select thread uses `snmp_select_info()` and `select()` to multiplex I/O across all active SNMP sessions
4. When responses arrive, `snmp_read()` processes them and invokes callbacks
5. Sessions are automatically cleaned up after callback execution

This provides:
- Lower memory footprint than thread-per-request
- Better scalability with many concurrent requests
- Reduced thread context switching overhead
- Native net-snmp async support

### SnmpSession Class (include/SnmpSession.h, src/SnmpSession.cpp)

Thread-safe synchronous SNMP session wrapper with mutex protection:

- **Single OID GET**: `get(const std::string& oid)` - blocking synchronous request
- **Multi-OID GET**: `getMulti(const std::vector<std::string>& oids)` - retrieve multiple OIDs in a single SNMP PDU
- **SET operations**: `set(const std::string& oid, char type, const std::string& value)`
- Uses `snmp_synch_response()` for blocking operations
- All methods are mutex-protected for thread safety
- RAII-based resource management (session closed in destructor)
- Non-copyable by design (deleted copy constructor/assignment)

Key implementation details:
- Each method acquires `mutex_` before accessing `sessionHandle_`
- Session initialization happens in `initializeSession()`, called from constructor
- Default timeout: 1000000 microseconds (1 second), 3 retries

### SnmpWorker Class (include/SnmpWorker.h, src/SnmpWorker.cpp)

Asynchronous SNMP operations using a single select thread:

- **Single select thread**: `selectThread_` runs the main I/O loop
- **Task queue**: `taskQueue_` holds pending SnmpTask objects
- **Active sessions**: `activeSessions_` tracks in-flight SNMP requests (map of session handle to SessionContext)
- **Callback mechanism**: Results delivered via `std::function` callbacks

Key methods:
- `start()` - Spawns the select thread
- `stop()` - Graceful shutdown, joins thread, closes all sessions
- `addTask(const SnmpTask& task)` - Queue a new SNMP request
- `wait()` - Block until all tasks complete (checks `taskQueue_.empty() && activeTasks_ == 0 && activeSessions_.empty()`)

Select thread workflow (src/SnmpWorker.cpp:263-295):
1. Dequeue task from `taskQueue_`
2. Call `processTask()` to create session and send async request with `snmp_send()`
3. Use `snmp_select_info()` to get file descriptors for all active sessions
4. Call `select()` to wait for I/O
5. When data arrives, call `snmp_read()` which triggers callbacks
6. `asyncCallback()` handles responses, cleans up sessions, decrements `activeTasks_`

### SnmpTask Structure (include/SnmpWorker.h:17-28)

Encapsulates a single async SNMP request:

- **Single-OID mode**: Set `isMultiOid = false`, populate `oid` and `callback`
- **Multi-OID mode**: Set `isMultiOid = true`, populate `oids` and `multiCallback`
- Common fields: `host`, `community`, `version` (default: SNMPv2c)

Callbacks:
- Single-OID: `std::function<void(const std::string& oid, const std::string& result)>`
- Multi-OID: `std::function<void(const std::vector<std::pair<std::string, std::string>>& results)>`

### Thread Safety Model

1. **SnmpSession**: All public methods protected by `mutex_`. Safe to call from any thread, but operations are serialized.
2. **SnmpWorker**:
   - `taskQueue_` protected by `queueMutex_`
   - `activeSessions_` protected by `sessionMutex_`
   - Each SNMP session is owned by a single `SessionContext` instance
   - Callbacks execute on the select thread - keep them fast and non-blocking
3. **Session isolation**: Each task creates its own `netsnmp_session` instance, no session sharing between tasks

### Error Handling

- Synchronous methods (SnmpSession): Return error strings prefixed with "ERROR:"
- Async methods (SnmpWorker): Deliver errors via callbacks with "ERROR:" prefix
- Common error scenarios handled:
  - Session open failure: "ERROR: Failed to open session"
  - Invalid OID: "ERROR: Invalid OID"
  - Timeout: "ERROR: Request timed out"
  - SNMP protocol errors: "ERROR: <snmp_errstring()>"

## Code Patterns

### Using SnmpSession for Synchronous Single-Host Operations

```cpp
SnmpSession session("192.168.1.1", "public");
if (session.open()) {
    // Single OID
    std::string result = session.get("1.3.6.1.2.1.1.1.0");

    // Multiple OIDs in one request
    auto results = session.getMulti({"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0"});

    session.close();
}
```

### Using SnmpWorker for Async Operations

```cpp
SnmpWorker worker;
worker.start();

// Single-OID task
SnmpTask task;
task.host = "192.168.1.1";
task.community = "public";
task.oid = "1.3.6.1.2.1.1.1.0";
task.isMultiOid = false;
task.callback = [](const std::string& oid, const std::string& result) {
    std::cout << oid << " -> " << result << std::endl;
};
worker.addTask(task);

// Multi-OID task
SnmpTask multiTask;
multiTask.host = "192.168.1.1";
multiTask.community = "public";
multiTask.oids = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.3.0"};
multiTask.isMultiOid = true;
multiTask.multiCallback = [](const auto& results) {
    for (const auto& [oid, value] : results) {
        std::cout << oid << " -> " << value << std::endl;
    }
};
worker.addTask(multiTask);

worker.wait();  // Block until all tasks complete
worker.stop();
```

## Common SNMP OIDs

| OID | Description | System MIB Name |
|-----|-------------|-----------------|
| 1.3.6.1.2.1.1.1.0 | System Description | sysDescr |
| 1.3.6.1.2.1.1.2.0 | System Object ID | sysObjectID |
| 1.3.6.1.2.1.1.3.0 | System Uptime | sysUpTime |
| 1.3.6.1.2.1.1.4.0 | System Contact | sysContact |
| 1.3.6.1.2.1.1.5.0 | System Name | sysName |
| 1.3.6.1.2.1.1.6.0 | System Location | sysLocation |
| 1.3.6.1.2.1.1.7.0 | System Services | sysServices |

## Important Implementation Notes

### When Modifying Thread Safety
- Never access `sessionHandle_` in SnmpSession without holding `mutex_`
- In SnmpWorker, acquire `sessionMutex_` before accessing `activeSessions_`
- The select thread is the ONLY thread that calls net-snmp I/O functions (`snmp_select_info()`, `snmp_read()`, `snmp_timeout()`)

### When Adding New Features
- Keep callbacks fast - they execute on the select thread and block other I/O
- Always clean up sessions: add entry to `activeSessions_` when sending, remove in callback
- Update `activeTasks_` counter correctly: increment in `addTask()`, decrement in callback/error paths
- Notify `completionCondition_` when `taskQueue_.empty() && activeTasks_ == 0`

### Memory Management
- `SessionContext` uses `std::shared_ptr` to safely reference from both select thread and callback
- SNMP PDUs: Created with `snmp_pdu_create()`, automatically freed by net-snmp on `snmp_send()` success, manually freed with `snmp_free_pdu()` on error
- Session peername/community: Must be dynamically allocated for `snmp_open()`, freed immediately after

### Build Configuration (CMakeLists.txt)
- Compiler flags: `-O0 -ggdb -Wall -Wextra -pthread` (debug build with warnings)
- Static library: `threading_snmp` contains SnmpSession and SnmpWorker
- Example executable: `snmp_thread_example` links against `threading_snmp`
