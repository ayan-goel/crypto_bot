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

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

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

# Add executable
set(SOURCES
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

add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries
target_link_libraries(${PROJECT_NAME}
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${HIREDIS_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
    z  # zlib for compression
    pthread
)

# Include directories for hiredis
target_include_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_INCLUDE_DIRS})

# Set compile flags for hiredis
target_compile_options(${PROJECT_NAME} PRIVATE ${HIREDIS_CFLAGS_OTHER})

# Include directories and flags for libwebsockets
target_include_directories(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_INCLUDE_DIRS})
target_compile_options(${PROJECT_NAME} PRIVATE ${LIBWEBSOCKETS_CFLAGS_OTHER})

# Installation
install(TARGETS ${PROJECT_NAME} DESTINATION bin) 