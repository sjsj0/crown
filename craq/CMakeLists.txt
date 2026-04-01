cmake_minimum_required(VERSION 3.16)
project(craq_standalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

add_library(craq_common
    src/protocol.cpp
    src/net.cpp
    src/store.cpp
    src/config.cpp
)

target_include_directories(craq_common PUBLIC include)

add_executable(craq_node
    src/node_main.cpp
    src/node_server.cpp
)
target_link_libraries(craq_node PRIVATE craq_common)

add_executable(craq_leader
    src/leader_main.cpp
)
target_link_libraries(craq_leader PRIVATE craq_common)
