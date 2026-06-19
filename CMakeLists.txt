cmake_minimum_required(VERSION 3.16)
project(nth VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# MSVC has different warning flags; POSIX uses GCC/Clang-style.
if(MSVC)
    add_compile_options(/W3 /utf-8 /EHsc)
    # JSC headers expect a relatively permissive compiler — disable a few
    # MSVC warnings that fire on them but are harmless.
    add_compile_options(/wd4267 /wd4244 /wd4146 /wd4018)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/SetupJavaScriptCore.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/SetupNlohmannJson.cmake)

set(NTH_SOURCES
    src/main.cpp
    src/cli/parser.cpp
    src/config/config.cpp
    src/chain/executor.cpp
    src/js/engine.cpp
    src/js/module_loader.cpp
    src/js/globals.cpp
    src/js/fetch.cpp
    src/js/http_server.cpp
    src/server/http.cpp
    src/server/websocket.cpp
    src/runtimes/role_resolver.cpp
    src/runtimes/pm2.cpp
    src/util/subprocess.cpp
    src/util/net.cpp
    src/util/sha1.cpp
)

add_executable(nth ${NTH_SOURCES})

target_include_directories(nth PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${JSC_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
)

# Platform-specific system libraries.
if(WIN32)
    target_link_libraries(nth PRIVATE
        ${JSC_LIBRARIES}
        ws2_32          # Winsock 2 — sockets, getaddrinfo, etc.
        bcrypt          # required by some JSC build configs on Win32
        advapi32        # for security-related calls in JSC
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(nth PRIVATE
        ${JSC_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        pthread
        dl
        rt
    )
endif()

# Install
install(TARGETS nth RUNTIME DESTINATION bin)
