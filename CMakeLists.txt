cmake_minimum_required(VERSION 3.22)
project(perCoreShard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    # 기본값은 RelWithDebInfo가 아니라 Debug: RelWithDebInfo/Release는
    # NDEBUG를 정의해서 GatewayShard/UpstreamManager의 assert() 기반
    # cross-thread 소유권 검증을 조용히 무력화시킨다. 이 프로젝트는
    # 아직 아키텍처 정확성을 검증하는 단계라 이 검증이 기본으로 켜져
    # 있어야 함 -- 그게 더 이상 우선순위가 아니게 되면 그때
    # -DCMAKE_BUILD_TYPE=Release를 명시적으로 지정할 것.
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

# 테스트: GTest가 없으면 조용히 건너뛰고 본체(perCoreShard)만 빌드한다.
# 테스트 대상(BufferPool/LocalMetrics/Config/UpstreamManager/Session)은
# net/ 인터페이스만 거치므로, 테스트 바이너리엔 Boost 구현체(net/boost/)가
# 전혀 필요 없다 -- tests/fakes/의 가짜 구현체로 대체.
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    add_executable(perCoreShard_tests
        src/upstream/upstream_manager.cpp
        tests/buffer_pool_test.cpp
        tests/local_metrics_test.cpp
        tests/config_test.cpp
        tests/upstream_manager_test.cpp
        tests/session_test.cpp
    )
    target_include_directories(perCoreShard_tests PRIVATE src tests)
    target_link_libraries(perCoreShard_tests PRIVATE
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(perCoreShard_tests)
else()
    message(STATUS "GTest not found -- skipping perCoreShard_tests target")
endif()
