cmake_minimum_required(VERSION 3.16)
project(starcolony LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 核心静态库：游戏规则 + 内容加载（content.cpp 是唯一包含 nlohmann/json 的 TU）
add_library(sc_core STATIC src/game.cpp src/content.cpp)
target_include_directories(sc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(sc_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_options(sc_core PRIVATE -Wall -Wextra)

add_executable(starcolony src/main.cpp src/ui.cpp)
target_link_libraries(starcolony PRIVATE sc_core)
target_compile_options(starcolony PRIVATE -Wall -Wextra)

add_executable(starcolony-rpc src/rpc_server.cpp)
target_link_libraries(starcolony-rpc PRIVATE sc_core)
target_compile_options(starcolony-rpc PRIVATE -Wall -Wextra)

# 测试 / 工具（与 Makefile 路径保持一致；tests/ 需在包含路径上以找到 ai.hpp）
set(SC_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(selftest tests/selftest.cpp)
target_include_directories(selftest PRIVATE ${SC_TEST_INCLUDES})
target_link_libraries(selftest PRIVATE sc_core)
target_compile_options(selftest PRIVATE -Wall -Wextra)

add_executable(qa_edge tests/edge_tests.cpp)
target_include_directories(qa_edge PRIVATE ${SC_TEST_INCLUDES})
target_link_libraries(qa_edge PRIVATE sc_core)
target_compile_options(qa_edge PRIVATE -Wall -Wextra)

add_executable(protocol_tests tests/protocol_tests.cpp)
target_include_directories(protocol_tests PRIVATE ${SC_TEST_INCLUDES})
target_link_libraries(protocol_tests PRIVATE sc_core)
target_compile_options(protocol_tests PRIVATE -Wall -Wextra)

add_executable(trace tools/trace.cpp)
target_include_directories(trace PRIVATE ${SC_TEST_INCLUDES})
target_link_libraries(trace PRIVATE sc_core)
target_compile_options(trace PRIVATE -Wall -Wextra)

add_executable(sweep tools/sweep.cpp)
target_include_directories(sweep PRIVATE ${SC_TEST_INCLUDES})
target_link_libraries(sweep PRIVATE sc_core)
target_compile_options(sweep PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME selftest COMMAND selftest)

install(TARGETS starcolony RUNTIME DESTINATION bin)
