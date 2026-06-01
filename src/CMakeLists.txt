# 源文件列表
set(SOURCES
    rpc_framework.cpp
    rpc_server.cpp
    rpc_client.cpp
    service_registry.cpp
    load_balancer.cpp
    circuit_breaker.cpp
    logger.cpp
    metrics.cpp
)

# 创建静态库
add_library(agent_rpc_framework STATIC ${SOURCES})

# 链接依赖
target_link_libraries(agent_rpc_framework 
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    pthread
    uuid
)

# 包含目录
target_include_directories(agent_rpc_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

# 编译选项
target_compile_options(agent_rpc_framework PRIVATE
    -Wall
    -Wextra
    -std=c++17
)

# 安装规则
install(TARGETS agent_rpc_framework
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/agent_rpc
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
