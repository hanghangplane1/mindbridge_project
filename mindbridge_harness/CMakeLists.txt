cmake_minimum_required(VERSION 3.15)

# MindBridge C++ Harness：可复用库 + gateway + benchmark
set(MINDBRIDGE_HARNESS_SOURCES
    src/model/ollama_model_client.cpp
    src/model/openai_compatible_model_client.cpp
    src/model/dashscope_model_client.cpp
    src/model/model_router.cpp
    src/rag/chroma_vector_store.cpp
    src/auth/auth_service.cpp
    src/harness/permission_checker.cpp
    src/harness/hook_executor.cpp
    src/harness/trace_recorder.cpp
    src/harness/tool_registry.cpp
    src/harness/mcp_quota.cpp
    src/harness/agent_loop.cpp
    src/harness/trajectory_analyzer.cpp
    src/harness/response_validator.cpp
    src/harness/token_counter.cpp
    src/harness/token_budget.cpp
    src/runtime/conversation_memory.cpp
    src/runtime/memory_scorer.cpp
    src/runtime/prompt_policy.cpp
    src/runtime/risk_policy.cpp
    src/runtime/run_store.cpp
    src/runtime/session_store.cpp
    src/runtime/prompt_prefix.cpp
    src/runtime/context_manager.cpp
    src/state/distributed_state_store.cpp
    src/storage/cloud_storage.cpp
    src/tools/core_tools.cpp
    src/multimodal/dashscope_qwen_multimodal_analyzer.cpp
    src/multimodal/mimo_multimodal_analyzer.cpp
    src/multimodal/multimodal_tools.cpp
    src/speech/dashscope_realtime_speech_recognizer.cpp
    src/speech/dashscope_cosyvoice_synthesizer.cpp
    src/speech/mimo_speech_synthesizer.cpp
    src/speech/speech_tools.cpp
    src/routing/registry_agent_router.cpp
    src/net/async_http_server.cpp
    src/net/http_client.cpp
    src/net/managed_endpoint_router.cpp
    src/net/websocket_client.cpp
    src/observability/ebpf_monitor.cpp
    src/observability/boundary_trace_correlator.cpp
    src/platform/platform_store.cpp
    src/platform/universal_event_mapper.cpp
    src/benchmark/benchmark_runner.cpp
)

add_library(mindbridge_harness STATIC ${MINDBRIDGE_HARNESS_SOURCES})

set(MINDBRIDGE_CLOUD_DEPS_ROOT "${CMAKE_SOURCE_DIR}/.deps/cloud_storage/stage/usr")
set(MINDBRIDGE_CLOUD_INCLUDE_DIRS "")
set(MINDBRIDGE_CLOUD_LIBS "")
set(MINDBRIDGE_HIREDIS_ROOT "${CMAKE_SOURCE_DIR}/.deps/hiredis/usr")
file(GLOB MINDBRIDGE_HIREDIS_LIB_CANDIDATES
    "${MINDBRIDGE_HIREDIS_ROOT}/lib/*/libhiredis.so*"
    "${MINDBRIDGE_HIREDIS_ROOT}/lib/libhiredis.so*")
if(EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/include/fastdfs/fdfs_client.h" AND
   EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/include/fastcommon/logger.h" AND
   (EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfdfsclient.so" OR
    EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib/libfdfsclient.so") AND
   (EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfastcommon.so" OR
    EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib/libfastcommon.so") AND
   EXISTS "${MINDBRIDGE_HIREDIS_ROOT}/include/hiredis/hiredis.h" AND
   MINDBRIDGE_HIREDIS_LIB_CANDIDATES)
    pkg_check_modules(MYSQLCLIENT mysqlclient)
    if(MYSQLCLIENT_FOUND)
        list(APPEND MINDBRIDGE_CLOUD_INCLUDE_DIRS
            "${MINDBRIDGE_CLOUD_DEPS_ROOT}/include"
            "${MINDBRIDGE_HIREDIS_ROOT}/include"
            ${MYSQLCLIENT_INCLUDE_DIRS})
        if(EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfdfsclient.so")
            list(APPEND MINDBRIDGE_CLOUD_LIBS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfdfsclient.so")
        else()
            list(APPEND MINDBRIDGE_CLOUD_LIBS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib/libfdfsclient.so")
        endif()
        if(EXISTS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfastcommon.so")
            list(APPEND MINDBRIDGE_CLOUD_LIBS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib64/libfastcommon.so")
        else()
            list(APPEND MINDBRIDGE_CLOUD_LIBS "${MINDBRIDGE_CLOUD_DEPS_ROOT}/lib/libfastcommon.so")
        endif()
        list(APPEND MINDBRIDGE_CLOUD_LIBS
            "${MINDBRIDGE_HIREDIS_LIB_CANDIDATES}"
            ${MYSQLCLIENT_LIBRARIES})
        target_compile_definitions(mindbridge_harness PRIVATE MINDBRIDGE_HAS_CLOUD_STORAGE_LIVE=1)
        message(STATUS "MindBridge cloud storage live adapters enabled")
    endif()
endif()

set(MINDBRIDGE_SYSTEM_OPENSSL_LIBS "")
if(EXISTS "/lib/x86_64-linux-gnu/libssl.so.1.1" AND EXISTS "/lib/x86_64-linux-gnu/libcrypto.so.1.1")
    list(APPEND MINDBRIDGE_SYSTEM_OPENSSL_LIBS
        "/lib/x86_64-linux-gnu/libssl.so.1.1"
        "/lib/x86_64-linux-gnu/libcrypto.so.1.1")
endif()

target_include_directories(mindbridge_harness
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/a2a/third_party>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/a2a/include
        ${CMAKE_SOURCE_DIR}/a2a/include/a2a/examples
        ${MINDBRIDGE_CLOUD_INCLUDE_DIRS}
)

target_link_libraries(mindbridge_harness
    PUBLIC
        a2a
        orchestrator
        CURL::libcurl
        Threads::Threads
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite::SQLite3
        ${MINDBRIDGE_CLOUD_LIBS}
        ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS}
)

if(NOT MSVC)
    target_compile_options(mindbridge_harness PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_gateway apps/gateway_main.cpp)
target_link_libraries(mindbridge_gateway PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_gateway PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_orchestrator apps/orchestrator_main.cpp)
target_link_libraries(mindbridge_orchestrator PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_orchestrator PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_counselor apps/counselor_main.cpp)
target_link_libraries(mindbridge_counselor PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_counselor PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_evaluator apps/evaluator_main.cpp)
target_link_libraries(mindbridge_evaluator PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_evaluator PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_benchmark apps/benchmark_main.cpp)
target_link_libraries(mindbridge_benchmark PRIVATE mindbridge_harness CURL::libcurl Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_benchmark PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_stress_test apps/stress_test_main.cpp)
target_link_libraries(mindbridge_stress_test PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_stress_test PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_net_demo apps/net_demo_main.cpp)
target_link_libraries(mindbridge_net_demo PRIVATE mindbridge_harness CURL::libcurl Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_net_demo PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_platform apps/platform_main.cpp)
target_link_libraries(mindbridge_platform PRIVATE mindbridge_harness SQLite::SQLite3 Threads::Threads Boost::system ${MINDBRIDGE_SYSTEM_OPENSSL_LIBS})
if(NOT MSVC)
    target_compile_options(mindbridge_platform PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_state_demo apps/state_demo_main.cpp)
target_link_libraries(mindbridge_state_demo PRIVATE mindbridge_harness SQLite::SQLite3 Threads::Threads)
if(NOT MSVC)
    target_compile_options(mindbridge_state_demo PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_ebpf_runtime_smoke apps/ebpf_runtime_smoke_main.cpp)
target_link_libraries(mindbridge_ebpf_runtime_smoke PRIVATE mindbridge_harness Threads::Threads)
if(NOT MSVC)
    target_compile_options(mindbridge_ebpf_runtime_smoke PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_platform_store_test apps/platform_store_test_main.cpp)
target_link_libraries(mindbridge_platform_store_test PRIVATE mindbridge_harness SQLite::SQLite3 Threads::Threads)
if(NOT MSVC)
    target_compile_options(mindbridge_platform_store_test PRIVATE -Wall -Wextra)
endif()

add_executable(mindbridge_response_validator_test apps/response_validator_test_main.cpp)
target_link_libraries(mindbridge_response_validator_test PRIVATE mindbridge_harness Threads::Threads)
if(NOT MSVC)
    target_compile_options(mindbridge_response_validator_test PRIVATE -Wall -Wextra)
endif()

option(MINDBRIDGE_ENABLE_EBPF "Build optional MindBridge eBPF monitor helper adapter" OFF)
if(MINDBRIDGE_ENABLE_EBPF)
    find_program(MINDBRIDGE_CLANG clang)
    find_program(MINDBRIDGE_BPFTOOL bpftool)
    pkg_check_modules(LIBBPF QUIET libbpf)
    set(MINDBRIDGE_EBPF_PROCESS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ebpf/agentsight_process")
    if(MINDBRIDGE_CLANG AND MINDBRIDGE_BPFTOOL AND LIBBPF_FOUND AND EXISTS "${MINDBRIDGE_EBPF_PROCESS_DIR}/Makefile")
        file(GLOB MINDBRIDGE_EBPF_PROCESS_HEADERS CONFIGURE_DEPENDS
            "${MINDBRIDGE_EBPF_PROCESS_DIR}/*.h"
            "${MINDBRIDGE_EBPF_PROCESS_DIR}/process_ext/*.h")
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_ebpf_monitor"
            COMMAND ${CMAKE_COMMAND} -E env
                    CLANG=${MINDBRIDGE_CLANG}
                    BPFTOOL=${MINDBRIDGE_BPFTOOL}
                    make -C "${MINDBRIDGE_EBPF_PROCESS_DIR}" process_new
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/process_new"
                    "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_ebpf_monitor"
            DEPENDS
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/process_new.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/process_new.bpf.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/process_new.h"
                    ${MINDBRIDGE_EBPF_PROCESS_HEADERS}
            COMMENT "Building MindBridge AgentSight-derived eBPF process monitor"
            VERBATIM)
        add_custom_target(mindbridge_ebpf_monitor ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_ebpf_monitor")
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_stdiocap_monitor"
            COMMAND ${CMAKE_COMMAND} -E env
                    CLANG=${MINDBRIDGE_CLANG}
                    BPFTOOL=${MINDBRIDGE_BPFTOOL}
                    make -C "${MINDBRIDGE_EBPF_PROCESS_DIR}" stdiocap
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/stdiocap"
                    "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_stdiocap_monitor"
            DEPENDS
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/stdiocap.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/stdiocap.bpf.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/stdiocap.h"
                    ${MINDBRIDGE_EBPF_PROCESS_HEADERS}
            COMMENT "Building MindBridge AgentSight-derived stdio capture monitor"
            VERBATIM)
        add_custom_target(mindbridge_stdiocap_monitor ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_stdiocap_monitor")
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_sslsniff_monitor"
            COMMAND ${CMAKE_COMMAND} -E env
                    CLANG=${MINDBRIDGE_CLANG}
                    BPFTOOL=${MINDBRIDGE_BPFTOOL}
                    make -C "${MINDBRIDGE_EBPF_PROCESS_DIR}" sslsniff
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/sslsniff"
                    "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_sslsniff_monitor"
            DEPENDS
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/sslsniff.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/sslsniff.bpf.c"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/sslsniff.h"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/container_info.h"
                    "${MINDBRIDGE_EBPF_PROCESS_DIR}/container_utils.h"
                    ${MINDBRIDGE_EBPF_PROCESS_HEADERS}
            COMMENT "Building MindBridge AgentSight-derived TLS plaintext monitor"
            VERBATIM)
        add_custom_target(mindbridge_sslsniff_monitor ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mindbridge_sslsniff_monitor")
    else()
        message(WARNING "MindBridge real eBPF helper dependencies not found; building placeholder adapter. Required: clang, bpftool, libbpf pkg-config.")
        add_executable(mindbridge_ebpf_monitor apps/ebpf_monitor_main.cpp)
        target_link_libraries(mindbridge_ebpf_monitor PRIVATE Threads::Threads)
        target_include_directories(mindbridge_ebpf_monitor PRIVATE ${CMAKE_SOURCE_DIR}/a2a/third_party)
        if(NOT MSVC)
            target_compile_options(mindbridge_ebpf_monitor PRIVATE -Wall -Wextra)
        endif()
        add_executable(mindbridge_stdiocap_monitor apps/ebpf_monitor_main.cpp)
        target_link_libraries(mindbridge_stdiocap_monitor PRIVATE Threads::Threads)
        target_include_directories(mindbridge_stdiocap_monitor PRIVATE ${CMAKE_SOURCE_DIR}/a2a/third_party)
        if(NOT MSVC)
            target_compile_options(mindbridge_stdiocap_monitor PRIVATE -Wall -Wextra)
        endif()
        add_executable(mindbridge_sslsniff_monitor apps/ebpf_monitor_main.cpp)
        target_link_libraries(mindbridge_sslsniff_monitor PRIVATE Threads::Threads)
        target_include_directories(mindbridge_sslsniff_monitor PRIVATE ${CMAKE_SOURCE_DIR}/a2a/third_party)
        if(NOT MSVC)
            target_compile_options(mindbridge_sslsniff_monitor PRIVATE -Wall -Wextra)
        endif()
    endif()
endif()
