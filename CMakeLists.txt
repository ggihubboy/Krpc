cmake_minimum_required(VERSION 3.16)
project(Krpc VERSION 1.0.0 LANGUAGES CXX)

option(KRPC_BUILD_EXAMPLES "Build the client and server examples" ON)
option(KRPC_BUILD_TESTS "Build Krpc tests" ON)
option(KRPC_ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# gcc 9 只认 c++2a，gcc 10+ 才有 -std=c++20
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    set(CMAKE_CXX20_STANDARD_COMPILE_OPTION "-std=c++2a")
    set(CMAKE_CXX20_EXTENSION_COMPILE_OPTION "-std=gnu++2a")
endif()

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

find_path(MUDUO_INCLUDE_DIR NAMES muduo/net/TcpServer.h)
find_library(MUDUO_NET_LIBRARY NAMES muduo_net)
find_library(MUDUO_BASE_LIBRARY NAMES muduo_base)
find_path(ZOOKEEPER_INCLUDE_DIR NAMES zookeeper/zookeeper.h)
find_library(ZOOKEEPER_LIBRARY NAMES zookeeper_mt)
find_path(GLOG_INCLUDE_DIR NAMES glog/logging.h)
find_library(GLOG_LIBRARY NAMES glog)

foreach(required_var
        MUDUO_INCLUDE_DIR
        MUDUO_NET_LIBRARY
        MUDUO_BASE_LIBRARY
        ZOOKEEPER_INCLUDE_DIR
        ZOOKEEPER_LIBRARY
        GLOG_INCLUDE_DIR
        GLOG_LIBRARY)
    if(NOT ${required_var})
        message(FATAL_ERROR
            "Required dependency ${required_var} was not found. "
            "See README.md for installation instructions.")
    endif()
endforeach()

add_library(krpc_project_options INTERFACE)
target_compile_options(krpc_project_options INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>)

if(KRPC_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "KRPC_ENABLE_ASAN requires GCC or Clang")
    endif()
    target_compile_options(krpc_project_options INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(krpc_project_options INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
endif()

set(KRPC_DEPENDENCIES
    protobuf::libprotobuf
    Threads::Threads
    ${ZOOKEEPER_LIBRARY}
    ${MUDUO_NET_LIBRARY}
    ${MUDUO_BASE_LIBRARY}
    ${GLOG_LIBRARY})

add_subdirectory(src)

if(KRPC_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if(KRPC_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
