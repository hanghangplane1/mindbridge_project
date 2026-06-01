cmake_minimum_required(VERSION 3.10)

add_library(email SHARED
    ${PROJECT_SOURCE_DIR}/plugins/email/Email.cpp
)

if(UNIX)
    set_target_properties(email PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

target_link_libraries(email PRIVATE CURL::libcurl Threads::Threads)
target_include_directories(email PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
