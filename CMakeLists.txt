cmake_minimum_required(VERSION 3.16)
project(starcolony LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(sc_core STATIC src/game.cpp)
target_include_directories(sc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sc_core PRIVATE -Wall -Wextra)

add_executable(starcolony src/main.cpp src/ui.cpp)
target_link_libraries(starcolony PRIVATE sc_core)
target_compile_options(starcolony PRIVATE -Wall -Wextra)

add_executable(starcolony-rpc src/rpc_server.cpp)
target_link_libraries(starcolony-rpc PRIVATE sc_core)
target_compile_options(starcolony-rpc PRIVATE -Wall -Wextra)

add_executable(selftest tests/selftest.cpp)
target_link_libraries(selftest PRIVATE sc_core)
target_compile_options(selftest PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME selftest COMMAND selftest)

install(TARGETS starcolony RUNTIME DESTINATION bin)
