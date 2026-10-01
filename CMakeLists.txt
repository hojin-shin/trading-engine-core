cmake_minimum_required(VERSION 3.24)
project(trading_engine_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)

option(TRADING_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
add_library(trading_options INTERFACE)
if(MSVC)
    target_compile_options(trading_options INTERFACE /W4 /permissive-)
else()
    target_compile_options(trading_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
if(TRADING_ENABLE_SANITIZERS)
    if(NOT UNIX OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require GCC or Clang on a Unix platform")
    endif()
    target_compile_options(trading_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(trading_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(trading_core
    src/types.cpp
    src/replay_market_data_source.cpp
    src/example_strategy.cpp
    src/cancel_example_strategy.cpp
    src/risk_manager.cpp
    src/order_manager.cpp
    src/simulated_exchange.cpp
    src/position_manager.cpp
    src/trading_engine.cpp)
target_include_directories(trading_core PUBLIC include)
target_link_libraries(trading_core PUBLIC Threads::Threads trading_options)
target_compile_features(trading_core PUBLIC cxx_std_20)

add_executable(trading_demo app/main.cpp)
target_link_libraries(trading_demo PRIVATE trading_core)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest 1.14 CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            URL https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.15.2
            URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(trading_tests
        tests/queue_tests.cpp
        tests/risk_order_tests.cpp
        tests/exchange_position_tests.cpp
        tests/replay_engine_tests.cpp
        tests/cancel_engine_tests.cpp)
    target_link_libraries(trading_tests PRIVATE trading_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(trading_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 15)
    add_test(NAME demo_cancel COMMAND trading_demo --trace --cancel-demo
        ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_cancel.csv)
    set_tests_properties(demo_cancel PROPERTIES
        PASS_REGULAR_EXPRESSION "order=1 state=Cancelled filled=0 average_price=0.00"
        TIMEOUT 15)
    add_test(NAME demo_cancel_partial COMMAND trading_demo --trace --cancel-demo
        ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_cancel_partial.csv)
    set_tests_properties(demo_cancel_partial PROPERTIES
        PASS_REGULAR_EXPRESSION "SYNTH position=1 realized_pnl=0.00 unrealized_pnl=-1.00"
        TIMEOUT 15)
    add_test(NAME demo_cancel_missing_csv COMMAND trading_demo --cancel-demo)
    add_test(NAME demo_conflicting_modes COMMAND trading_demo --limit-demo --cancel-demo
        ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_cancel.csv)
    set_tests_properties(demo_cancel_missing_csv demo_conflicting_modes PROPERTIES WILL_FAIL TRUE TIMEOUT 15)
    add_test(NAME demo_limit COMMAND trading_demo --trace --limit-demo
        ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_limit.csv)
    set_tests_properties(demo_limit PROPERTIES
        PASS_REGULAR_EXPRESSION "SYNTH position=0 realized_pnl=8.00 unrealized_pnl=0.00"
        TIMEOUT 15)
    add_test(NAME demo_limit_missing_csv COMMAND trading_demo --limit-demo)
    set_tests_properties(demo_limit_missing_csv PROPERTIES WILL_FAIL TRUE TIMEOUT 15)
    add_test(NAME demo_builtin COMMAND trading_demo)
    add_test(NAME demo_trace COMMAND trading_demo --trace)
    set_tests_properties(demo_trace PROPERTIES
        PASS_REGULAR_EXPRESSION "event=ORDER order=1 symbol=SYNTH side=Buy type=Market state=Accepted"
        TIMEOUT 15)
    add_test(NAME demo_csv COMMAND trading_demo ${CMAKE_CURRENT_SOURCE_DIR}/data/sample.csv)
    set_tests_properties(demo_builtin demo_csv PROPERTIES
        PASS_REGULAR_EXPRESSION "SYNTH position=0 realized_pnl=6.00 unrealized_pnl=0.00"
        TIMEOUT 15)
endif()
