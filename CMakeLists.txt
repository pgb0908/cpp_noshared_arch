cmake_minimum_required(VERSION 3.22)
project(perCoreShard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    # Debug (not RelWithDebInfo) by default: RelWithDebInfo/Release define
    # NDEBUG, which silently disables the assert()-based cross-thread
    # ownership checks in GatewayShard/UpstreamManager. This project is
    # still verifying architectural correctness, so those checks should be
    # live by default -- pass -DCMAKE_BUILD_TYPE=Release explicitly once
    # that's no longer the priority.
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(perCoreShard
    main.cpp
    src/runtime/gateway_shard.cpp
    src/runtime/gateway_runtime.cpp
    src/upstream/upstream_manager.cpp
    src/net/boost/acceptor.cpp
    src/net/boost/event_loop.cpp
    src/net/boost/factory.cpp
    src/net/boost/resolver.cpp
    src/net/boost/socket.cpp
    src/net/boost/timer.cpp
)

target_include_directories(perCoreShard PRIVATE src)
target_link_libraries(perCoreShard PRIVATE Boost::headers Boost::system Threads::Threads nlohmann_json::nlohmann_json)
target_compile_definitions(perCoreShard PRIVATE BOOST_ASIO_NO_DEPRECATED)
