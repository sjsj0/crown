cmake_minimum_required(VERSION 3.20)
project(crown CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Dependencies
# ============================================================

# Let gRPC pull in protobuf itself — do NOT call find_package(Protobuf)
# separately, it causes target redefinition conflicts on Homebrew.
find_package(gRPC REQUIRED)
find_package(nlohmann_json REQUIRED)

# Convenient aliases
set(PROTOC      $<TARGET_FILE:protobuf::protoc>)
set(GRPC_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

# ============================================================
# Proto code generation
# ============================================================

set(PROTO_DIR     "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_FILE    "${PROTO_DIR}/chain.proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/proto_gen")

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_SRCS "${PROTO_GEN_DIR}/chain.pb.cc")
set(PROTO_HDRS "${PROTO_GEN_DIR}/chain.pb.h")
set(GRPC_SRCS  "${PROTO_GEN_DIR}/chain.grpc.pb.cc")
set(GRPC_HDRS  "${PROTO_GEN_DIR}/chain.grpc.pb.h")

add_custom_command(
    OUTPUT  ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC}
            --proto_path=${PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_PLUGIN}
            ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf and gRPC sources from chain.proto"
)

add_library(chain_proto STATIC
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)
target_include_directories(chain_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(chain_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

# ============================================================
# Chain replication executable
# ============================================================
add_library(replication STATIC
    src/replication/replication_strategy.cpp
    src/replication/chain/chain_replication.cpp
    src/replication/craq/craq_replication.cpp
    src/replication/crown/crown_replication.cpp
)

target_include_directories(replication PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_GEN_DIR}
)

target_link_libraries(replication PUBLIC
    chain_proto
)

# ============================================================
# Server executable
# ============================================================

add_executable(server
    src/server/server.cpp
)
target_include_directories(server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_GEN_DIR}
)
target_link_libraries(server PRIVATE
    chain_proto
    replication
)

# ============================================================
# Client executable
# ============================================================

add_executable(client
    src/client/client.cpp
)
target_include_directories(client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_GEN_DIR}
)
target_link_libraries(client PRIVATE
    chain_proto
    nlohmann_json::nlohmann_json
)
