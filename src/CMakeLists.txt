cmake_minimum_required(VERSION 3.16)
project(kimi_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(kimi_client kimi_client.cpp)

target_link_libraries(kimi_client
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

target_compile_options(kimi_client PRIVATE -Wall -Wextra -O2 -g)
