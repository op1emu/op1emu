cmake_minimum_required(VERSION 3.20)

project(op1emu)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

# Fetch spdlog
message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.11.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "spdlog fetched successfully")

# Fetch GLFW
message(STATUS "Fetching GLFW...")
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL " " FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL " " FORCE)
set(GLFW_INSTALL OFF CACHE BOOL " " FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)
message(STATUS "GLFW fetched successfully")
find_package(OpenGL REQUIRED)

# Fetch nlohmann/json
message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)
message(STATUS "nlohmann/json fetched successfully")

# Fetch stb (for image loading)
message(STATUS "Fetching stb...")
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(stb)
message(STATUS "stb fetched successfully")

# Fetch cqueue
message(STATUS "Fetching cqueue...")
FetchContent_Declare(cqueue
  GIT_REPOSITORY https://github.com/torrentg/cqueue.git
  GIT_TAG        1.0.6
)
FetchContent_MakeAvailable(cqueue)
message(STATUS "cqueue fetched successfully")
add_library(cqueue INTERFACE)
target_include_directories(cqueue INTERFACE ${cqueue_SOURCE_DIR})

# Fetch uvw
message(STATUS "Fetching uvw...")
FetchContent_Declare(
    uvw
    GIT_REPOSITORY https://github.com/skypjack/uvw.git
    GIT_TAG        v3.4.0_libuv_v1.48
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_UVW_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(uvw)
message(STATUS "uvw fetched successfully")

# Fetch miniaudio (single-header audio library)
message(STATUS "Fetching miniaudio...")
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniaudio)
message(STATUS "miniaudio fetched successfully")

find_package(Threads REQUIRED)

# Measurement build (docs/profiling.md). OFF compiles every profiling hook out
# of the emulator and of bcore, so a normal build carries none of it.
option(ENABLE_PROFILING "Build the profiling hooks: phase marks, trace, census, perf/jitdump" OFF)
set(BCORE_ENABLE_PROFILE ${ENABLE_PROFILING} CACHE BOOL "bcore observability API (follows ENABLE_PROFILING)" FORCE)
set(BCORE_PERF_JIT_EVENTS ${ENABLE_PROFILING} CACHE BOOL "bcore jitdump support (follows ENABLE_PROFILING)" FORCE)
if(ENABLE_PROFILING)
    # perf call graphs through host code need frame pointers.
    add_compile_options(-fno-omit-frame-pointer)
endif()

add_subdirectory(ext/bcore EXCLUDE_FROM_ALL)

file(GLOB SOURCES ext/cqueue/*.cpp)
add_library(ext ${SOURCES})
# use C++20 for ext
set_target_properties(ext PROPERTIES CXX_STANDARD 20)
target_include_directories(ext PUBLIC ext)
target_link_libraries(ext PRIVATE cqueue)

file(GLOB_RECURSE SOURCES src/*.cpp)
if(NOT ENABLE_PROFILING)
    list(FILTER SOURCES EXCLUDE REGEX "/src/profiling/")
endif()
add_library(emulator ${SOURCES})
target_include_directories(emulator PUBLIC src)
target_include_directories(emulator PRIVATE ext)
target_link_libraries(emulator PRIVATE bfin-core spdlog::spdlog)
if(ENABLE_PROFILING)
    target_compile_definitions(emulator PUBLIC ENABLE_PROFILING)
    target_link_libraries(emulator PRIVATE nlohmann_json::nlohmann_json)
endif()

add_executable(ldrdump tools/ldrdump.cpp)
target_link_libraries(ldrdump emulator)

file(GLOB_RECURSE SOURCES gui/*.cpp)
add_executable(op1emu ${SOURCES})
target_link_libraries(op1emu PRIVATE emulator glfw OpenGL::GL nlohmann_json::nlohmann_json uvw ext Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(op1emu PRIVATE ${stb_SOURCE_DIR} ${miniaudio_SOURCE_DIR})

option(OP1_BUILD_HEADLESS_TESTS "Build headless frontend regressions" OFF)
if(OP1_BUILD_HEADLESS_TESTS)
    enable_testing()
    add_executable(op1-headless-test tests/headless_test.cpp gui/headless.cpp)
    target_include_directories(op1-headless-test PRIVATE gui)
    target_link_libraries(op1-headless-test PRIVATE emulator nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME op1-headless-test COMMAND op1-headless-test)
    set_tests_properties(op1-headless-test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(op1-time-source-test tests/time_source_test.cpp)
    target_include_directories(op1-time-source-test PRIVATE src)
    add_test(NAME op1-time-source-test COMMAND op1-time-source-test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME op1-ldr-smoke
            COMMAND ${Python3_EXECUTABLE} tests/ldr_smoke.py $<TARGET_FILE:op1emu> $<TARGET_FILE:ldrdump>
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()

option(OP1_BUILD_DMA_TESTS "Build firmware-free DMA regressions" OFF)
if(OP1_BUILD_DMA_TESTS)
    enable_testing()
    add_executable(op1-dma-descriptor-test tests/dma_descriptor_test.cpp)
    target_link_libraries(op1-dma-descriptor-test PRIVATE emulator)
    add_test(NAME op1-dma-descriptor-test COMMAND op1-dma-descriptor-test)
endif()
