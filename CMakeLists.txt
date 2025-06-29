cmake_minimum_required(VERSION 3.12)
project(crypto_hft_bot VERSION 1.0.0 LANGUAGES CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set compiler flags for optimization and debugging
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add jwt-cpp subdirectory
add_subdirectory(jwt-cpp)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/jwt-cpp/include)

# macOS specific paths for Homebrew
if(APPLE)
    include_directories(/opt/homebrew/include)
    link_directories(/opt/homebrew/lib)
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
endif()

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# Find hiredis (Redis C client)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# Find libwebsockets
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# Add executables
set(LEGACY_SOURCES
    src/main.cpp
    src/config.cpp
    src/logger.cpp
    src/order_book.cpp
    src/strategy.cpp
    src/order_manager.cpp
    src/websocket_client.cpp
    src/rest_client.cpp
    src/risk_manager.cpp
)

set(HFT_SOURCES
    src/hft_main.cpp
    src/hft_engine.cpp
    src/config.cpp
    src/logger.cpp
    src/order_manager.cpp
    src/risk_manager.cpp
    src/websocket_client.cpp
    src/rest_client.cpp
    src/order_book.cpp
)

# Legacy bot (original architecture)
add_executable(${PROJECT_NAME} ${LEGACY_SOURCES})

# New HFT engine (ultra high-frequency)
add_executable(crypto_hft_engine ${HFT_SOURCES})

# Link libraries for both executables
target_link_libraries(${PROJECT_NAME}
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    ${HIREDIS_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
    z  # zlib for compression
    pthread
)

target_link_libraries(crypto_hft_engine
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    ${HIREDIS_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
    z  # zlib for compression
    pthread
)

# Include directories for hiredis
target_include_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_INCLUDE_DIRS})
target_include_directories(crypto_hft_engine PRIVATE ${HIREDIS_INCLUDE_DIRS})

# Set compile flags for hiredis
target_compile_options(${PROJECT_NAME} PRIVATE ${HIREDIS_CFLAGS_OTHER})
target_compile_options(crypto_hft_engine PRIVATE ${HIREDIS_CFLAGS_OTHER})

# Include directories and flags for libwebsockets
target_include_directories(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_INCLUDE_DIRS})
target_include_directories(crypto_hft_engine PRIVATE ${LIBWEBSOCKETS_INCLUDE_DIRS})
target_compile_options(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})
target_compile_options(crypto_hft_engine PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

# Installation
install(TARGETS ${PROJECT_NAME} DESTINATION bin) 