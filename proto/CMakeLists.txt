# 生成protobuf和gRPC代码
find_program(PROTOC_EXECUTABLE protoc
    HINTS "${CMAKE_SOURCE_DIR}/.deps/grpc/bin")
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin
    HINTS "${CMAKE_SOURCE_DIR}/.deps/grpc/bin")

if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc not found")
endif()
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

set(PROTO_FILES
    common.proto
    agent_service.proto
    ai_query.proto
)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    
    # 生成protobuf代码
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        COMMENT "Generating protobuf code for ${PROTO_FILE}"
    )
    
    # 生成gRPC代码
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
             --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        COMMENT "Generating gRPC code for ${PROTO_FILE}"
    )
    
    list(APPEND PROTO_SOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc
    )
    
    list(APPEND PROTO_HEADERS
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h
    )
endforeach()

# 创建proto库
add_library(proto_lib STATIC ${PROTO_SOURCES} ${PROTO_HEADERS})
target_link_libraries(proto_lib ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES})
target_include_directories(proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# 导出头文件
install(FILES ${PROTO_HEADERS} DESTINATION include/proto)
