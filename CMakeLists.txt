cmake_minimum_required(VERSION 2.8.12)
 project(hopey)

 add_definitions("-std=c++2a")

 include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
 conan_basic_setup()

 add_executable(hopey main.cpp)
 target_link_libraries(hopey ${CONAN_LIBS})
