cmake_minimum_required(VERSION 3.12)
project(crypto_hft_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Wpedantic -Wunused")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wunused")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(jwt-cpp)

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/jwt-cpp/include)

if(APPLE)
    include_directories(/opt/homebrew/include)
    link_directories(/opt/homebrew/lib)
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

set(HFT_SOURCES
    src/main.cpp
    src/engine.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/data/websocket_client.cpp
    src/data/market_data_feed.cpp
    src/strategy/market_maker.cpp
    src/execution/executor.cpp
    src/order/order_manager.cpp
    src/risk/risk_manager.cpp
    src/metrics/metrics.cpp
)

add_executable(${PROJECT_NAME} ${HFT_SOURCES})

target_link_libraries(${PROJECT_NAME}
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    ${LIBWEBSOCKETS_LIBRARIES}
    z
    pthread
)

target_include_directories(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_INCLUDE_DIRS})
target_compile_options(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

set(TEST_SOURCES
    tests/smoke_test.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/strategy/market_maker.cpp
    src/execution/executor.cpp
    src/order/order_manager.cpp
    src/risk/risk_manager.cpp
    src/metrics/metrics.cpp
)

add_executable(smoke_test ${TEST_SOURCES})
target_link_libraries(smoke_test
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    pthread
)
target_include_directories(smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
