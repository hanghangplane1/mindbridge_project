cmake_minimum_required(VERSION 3.10)

add_library(excel SHARED
    ${PROJECT_SOURCE_DIR}/plugins/excel/Excel.cpp
)

if(UNIX)
    set_target_properties(excel PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Threads REQUIRED)
target_link_libraries(excel PRIVATE Threads::Threads)

target_include_directories(excel PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
