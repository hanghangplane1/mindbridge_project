# 查找Google Test
find_package(GTest REQUIRED)

# 注意: test_rpc_framework 暂时禁用，需要更新以匹配新的模块结构
# TODO: 修复 test_rpc_framework.cpp 中的类型引用问题
# add_executable(test_rpc_framework test_rpc_framework.cpp)
# target_link_libraries(test_rpc_framework ...)
# target_include_directories(test_rpc_framework ...)

# 序列化测试
add_executable(test_serialization test_serialization.cpp)

# 链接库
target_link_libraries(test_serialization 
    agent_rpc_common
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
    pthread
    uuid
)

# 包含目录
target_include_directories(test_serialization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

# A2A集成测试
add_executable(test_a2a_integration test_a2a_integration.cpp)

target_link_libraries(test_a2a_integration 
    a2a
    GTest::gtest
    GTest::gtest_main
    pthread
    CURL::libcurl
)

target_include_directories(test_a2a_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
)

# 添加测试
enable_testing()
# 注意: RpcFrameworkTest 已禁用，不再注册
add_test(NAME SerializationTest COMMAND test_serialization)
add_test(NAME A2AIntegrationTest COMMAND test_a2a_integration)

# Proto序列化往返属性测试
# **Feature: a2a-integration, Property 11: Protobuf Serialization Round-Trip**
# **Validates: Requirements 11.5**
find_package(rapidcheck REQUIRED)

add_executable(test_proto_roundtrip test_proto_roundtrip.cpp)

target_link_libraries(test_proto_roundtrip 
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
)

target_include_directories(test_proto_roundtrip PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

add_test(NAME ProtoRoundTripTest COMMAND test_proto_roundtrip)

# A2A适配器属性测试
# **Feature: a2a-integration, Property 1, 7, 10**
# **Validates: Requirements 5.1, 5.5, 8.1-8.4, 9.5, 10.1**
add_executable(test_adapter_properties test_adapter_properties.cpp)

target_link_libraries(test_adapter_properties 
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
    CURL::libcurl
)

target_include_directories(test_adapter_properties PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a_adapter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

add_test(NAME AdapterPropertiesTest COMMAND test_adapter_properties)

# Task Manager属性测试
# **Feature: a2a-integration, Property 2, 3, 6**
# **Validates: Requirements 3.2, 3.5, 4.2, 5.5, 6.1-6.6**
add_executable(test_task_manager_properties test_task_manager_properties.cpp)

target_link_libraries(test_task_manager_properties 
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
    CURL::libcurl
)

target_include_directories(test_task_manager_properties PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a_adapter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

add_test(NAME TaskManagerPropertiesTest COMMAND test_task_manager_properties)

# Agent Router属性测试
# **Feature: a2a-integration, Property 4, 8**
# **Validates: Requirements 2.3, 3.4, 4.5, 10.3**
add_executable(test_agent_router_properties test_agent_router_properties.cpp)

target_link_libraries(test_agent_router_properties 
    orchestrator
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
    CURL::libcurl
)

target_include_directories(test_agent_router_properties PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a_adapter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

add_test(NAME AgentRouterPropertiesTest COMMAND test_agent_router_properties)

# 安装测试
install(TARGETS test_serialization
    RUNTIME DESTINATION bin
)


# AI Query集成测试
# **Feature: a2a-integration, Task 17: 集成测试**
# **Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 9.5, 10.2, 10.5**
add_executable(test_ai_query_integration test_ai_query_integration.cpp)

target_link_libraries(test_ai_query_integration 
    agent_rpc_server
    agent_rpc_client
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
    uuid
    CURL::libcurl
)

target_include_directories(test_ai_query_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a_adapter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

add_test(NAME AIQueryIntegrationTest COMMAND test_ai_query_integration)

# MCP Agent集成测试
# **Feature: a2a-integration, Task 20: MCP 集成测试**
# **Validates: Requirements 12.1, 12.3, 12.5**
add_executable(test_mcp_integration test_mcp_integration.cpp)

target_link_libraries(test_mcp_integration 
    agent_rpc_mcp
    agent_rpc_common
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
)

target_include_directories(test_mcp_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

add_test(NAME MCPIntegrationTest COMMAND test_mcp_integration)

# RAG-MCP 属性测试
# **Feature: rag-mcp, Property 1-8**
# **Validates: Requirements 1.3, 3.2, 3.3, 3.4, 4.1, 4.3, 7.2, 7.3**
add_executable(test_rag_mcp_properties test_rag_mcp_properties.cpp)

target_link_libraries(test_rag_mcp_properties 
    agent_rpc_mcp
    agent_rpc_common
    rapidcheck
    GTest::gtest
    GTest::gtest_main
    pthread
    CURL::libcurl
)

target_include_directories(test_rag_mcp_properties PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

add_test(NAME RAGMCPPropertiesTest COMMAND test_rag_mcp_properties)

# CaffeineCache 单元测试
add_executable(test_caffeine_cache test_caffeine_cache.cpp)

target_link_libraries(test_caffeine_cache
    GTest::gtest
    GTest::gtest_main
    pthread
    Threads::Threads
)

target_include_directories(test_caffeine_cache PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcp/include
)

add_test(NAME CaffeineCacheTest COMMAND test_caffeine_cache)
