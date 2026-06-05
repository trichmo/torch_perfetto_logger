cmake_minimum_required(VERSION 3.16)
project(perfetto_logger CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PERFETTO_SDK_DIR "/root/workspace/code/perfetto_sdk")
set(KINETO_INCLUDE_DIR "/root/workspace/code/kineto/libkineto/include")

# --- Perfetto SDK (static library from amalgamated source) ---
add_library(perfetto_sdk STATIC ${PERFETTO_SDK_DIR}/perfetto.cc)
target_include_directories(perfetto_sdk PUBLIC ${PERFETTO_SDK_DIR})
target_link_libraries(perfetto_sdk PRIVATE pthread)

# --- fmt (required by Kineto's GenericTraceActivity.h) ---
include(FetchContent)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1)
FetchContent_MakeAvailable(fmt)

# --- PerfettoLogger library ---
add_library(perfetto_logger STATIC src/PerfettoLogger.cpp)
target_include_directories(perfetto_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${KINETO_INCLUDE_DIR})
target_compile_definitions(perfetto_logger PUBLIC KINETO_NAMESPACE=libkineto)
target_link_libraries(perfetto_logger PUBLIC perfetto_sdk fmt::fmt)

# --- Test binary ---
add_executable(test_perfetto_logger test/test_perfetto_logger.cpp)
target_link_libraries(test_perfetto_logger PRIVATE perfetto_logger)
