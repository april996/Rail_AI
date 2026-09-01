cmake_minimum_required(VERSION 3.10)
project(cabin_core)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cabin STATIC
    src/CabinProtocol.cpp
    src/RoleRouter.cpp
    src/SessionManager.cpp
    src/PttController.cpp
)

target_include_directories(cabin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(cabin PUBLIC Threads::Threads)

install(TARGETS cabin DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
