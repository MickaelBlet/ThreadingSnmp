cmake_minimum_required(VERSION 3.10)
project(ThreadingSnmp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

find_path(NETSNMP_INCLUDE_DIR
    NAMES net-snmp/net-snmp-config.h
    PATHS /usr/include /usr/local/include
)

find_library(NETSNMP_LIBRARY
    NAMES netsnmp
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

if(NOT NETSNMP_INCLUDE_DIR OR NOT NETSNMP_LIBRARY)
    message(FATAL_ERROR "net-snmp library not found. Please install libsnmp-dev (Debian/Ubuntu) or net-snmp-devel (RedHat/CentOS)")
endif()

message(STATUS "Found net-snmp include: ${NETSNMP_INCLUDE_DIR}")
message(STATUS "Found net-snmp library: ${NETSNMP_LIBRARY}")

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${NETSNMP_INCLUDE_DIR}
)

add_library(threading_snmp STATIC
    src/SnmpWorker.cpp
    src/SnmpSession.cpp
)

target_link_libraries(threading_snmp
    PUBLIC
    ${NETSNMP_LIBRARY}
    Threads::Threads
    crypto
)

add_executable(snmp_thread_example
    examples/main.cpp
)

target_link_libraries(snmp_thread_example
    PRIVATE
    threading_snmp
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -ggdb -Wall -Wextra -pthread")
