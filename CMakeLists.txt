cmake_minimum_required(VERSION 3.22)
project(perCoreShard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(perCoreShard
    main.cpp
    src/gateway_shard.cpp
    src/gateway_runtime.cpp
    src/upstream_manager.cpp
)

target_include_directories(perCoreShard PRIVATE src)
target_link_libraries(perCoreShard PRIVATE Boost::headers Boost::system Threads::Threads nlohmann_json::nlohmann_json)
target_compile_definitions(perCoreShard PRIVATE BOOST_ASIO_NO_DEPRECATED)
