cmake_minimum_required(VERSION 3.15)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS hiredis)

if(NOT HIREDIS_FOUND)
    set(LOCAL_HIREDIS_ROOT "${CMAKE_SOURCE_DIR}/.deps/hiredis/usr")
    find_path(HIREDIS_INCLUDE_DIR
        NAMES hiredis/hiredis.h
        HINTS "${LOCAL_HIREDIS_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(HIREDIS_LIBRARY
        NAMES hiredis libhiredis.a
        HINTS
            "${LOCAL_HIREDIS_ROOT}/lib"
            "${LOCAL_HIREDIS_ROOT}/lib/x86_64-linux-gnu"
            "${LOCAL_HIREDIS_ROOT}/lib/aarch64-linux-gnu"
        NO_DEFAULT_PATH)
    if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
        set(HIREDIS_INCLUDE_DIRS "${HIREDIS_INCLUDE_DIR}")
        set(HIREDIS_LIBRARIES "${HIREDIS_LIBRARY}")
        set(HIREDIS_FOUND TRUE)
        message(STATUS "Using project-local hiredis: ${HIREDIS_LIBRARY}")
    endif()
endif()

if(NOT HIREDIS_FOUND)
    message(FATAL_ERROR "hiredis not found. Install libhiredis-dev or provide .deps/hiredis/usr.")
endif()

add_executable(ai_counselor_agent
    counselor_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_counselor_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_counselor_agent
    a2a
    mindbridge_harness
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

add_executable(ai_evaluator_agent
    evaluator_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_evaluator_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_evaluator_agent
    a2a
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

add_executable(ai_mindcare_client
    client_main.cpp
)

target_include_directories(ai_mindcare_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
)

target_link_libraries(ai_mindcare_client
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
)
