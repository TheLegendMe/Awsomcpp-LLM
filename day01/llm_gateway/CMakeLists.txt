cmake_minimum_required(VERSION 3.20)
project(llm_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── 依赖 ──────────────────────────────────────────────────────────
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

# Drogon (MIT, C++17/20 HTTP framework)
find_package(Drogon QUIET)
if(NOT Drogon_FOUND)
    # 尝试 pkg-config 查找
    pkg_check_modules(DROGON IMPORTED_TARGET libdrogon)
    if(NOT DROGON_FOUND)
        message(FATAL_ERROR "Drogon not found. Install: https://github.com/drogonframework/drogon")
    endif()
    set(DROGON_LIBS PkgConfig::DROGON)
else()
    set(DROGON_LIBS Drogon::Drogon)
endif()

# Lua 5.3 (优先使用 vendored 头文件)
find_package(Lua QUIET)
if(NOT LUA_FOUND)
    pkg_check_modules(LUA QUIET lua5.3)
    if(LUA_FOUND)
        set(LUA_INCLUDE_DIR ${LUA_INCLUDE_DIRS})
        set(LUA_LIBRARIES ${LUA_LIBRARIES})
    endif()
endif()

# 如果系统没有 Lua 头文件，使用 vendored 头文件
if(NOT LUA_INCLUDE_DIR AND NOT LUA_INCLUDE_DIRS)
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/lua.h)
        set(LUA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)
        message(STATUS "Using vendored Lua headers from third_party/")
    endif()
endif()

# 查找 Lua 运行时库
if(NOT LUA_LIBRARIES)
    find_library(LUA_LIB lua5.3 lua-5.3 lua
        HINTS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib)
    if(LUA_LIB)
        set(LUA_LIBRARIES ${LUA_LIB})
    else()
        message(FATAL_ERROR "Lua 5.3 library not found. Install liblua5.3-0 / liblua5.3-dev")
    endif()
endif()

# sol2 (header-only, vendored 或系统安装)
find_path(SOL2_INCLUDE_DIR sol/sol.hpp
    HINTS ${CMAKE_SOURCE_DIR}/third_party
          ${CMAKE_SOURCE_DIR}/third_party/sol2/include
          /opt/sol2/include
          /usr/local/include
          /usr/include)

if(NOT SOL2_INCLUDE_DIR)
    message(FATAL_ERROR "sol2 not found. Place sol/ headers in third_party/")
endif()

# libevent / pthread (Drogon 内部需要)
pkg_check_modules(LIBEVENT REQUIRED libevent libevent_pthreads)
find_package(Threads REQUIRED)

# libdl (用于 dlopen llama.cpp)
find_library(LIBDL dl)

# ─── 可执行文件 ────────────────────────────────────────────────────
add_executable(llm_gateway
    src/main.cpp
    src/plugin_manager.cpp
    src/lua_sandbox.cpp
    src/lua_vm_pool.cpp
    src/remote_upstream.cpp
    src/ws_chat_controller.cpp
)

target_include_directories(llm_gateway PRIVATE
    include
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    ${LUA_INCLUDE_DIRS}
    ${LIBEVENT_INCLUDE_DIRS}
)

target_link_libraries(llm_gateway PRIVATE
    ${DROGON_LIBS}
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${LUA_LIBRARIES}
    ${LIBEVENT_LIBRARIES}
    Threads::Threads
    ${LIBDL}
)

target_compile_options(llm_gateway PRIVATE
    -Wall -Wextra -O2 -g
    ${LIBEVENT_CFLAGS_OTHER}
)

# Drogon 需要 trantor（由 Drogon 自动链接）
# 如果 Drogon 未通过 CMake Config 找到，手动添加
if(NOT Drogon_FOUND AND DROGON_FOUND)
    target_link_libraries(llm_gateway PRIVATE ${DROGON_LIBRARIES})
endif()

# 安装
install(TARGETS llm_gateway RUNTIME DESTINATION bin)
install(DIRECTORY plugins/ DESTINATION share/llm_gateway/plugins)
install(DIRECTORY conf/ DESTINATION share/llm_gateway/conf)
