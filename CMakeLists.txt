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
# libuv's include(CTest) would turn BUILD_TESTING on, and with it uvw's own
# tests (googletest downloaded, built and registered with op1emu's). op1emu's
# tests do not use it.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
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

# mimalloc as the process's malloc. LLVM allocates and frees for every IR
# value, machine instruction and MC fragment it makes while compiling a
# block: with glibc's allocator, malloc and free were 8% of a boot's JIT
# compilation, and mimalloc took 6% of its instructions away. OFF keeps the C
# library's allocator.
option(OP1_MIMALLOC "Link mimalloc as the process's malloc" ON)
if(OP1_MIMALLOC)
    message(STATUS "Fetching mimalloc...")
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG v2.1.7
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(mimalloc)
    message(STATUS "mimalloc fetched successfully")
endif()

find_package(Threads REQUIRED)

# Measurement build (docs/profiling.md). OFF compiles every profiling hook out
# of the emulator and of bcore, so a normal build carries none of it.
option(ENABLE_PROFILING "Build the profiling hooks: phase marks, trace, census, perf/jitdump" OFF)
set(BCORE_ENABLE_PROFILE ${ENABLE_PROFILING} CACHE BOOL "bcore observability API (follows ENABLE_PROFILING)" FORCE)
set(BCORE_PERF_JIT_EVENTS ${ENABLE_PROFILING} CACHE BOOL "bcore jitdump support (follows ENABLE_PROFILING)" FORCE)
if(ENABLE_PROFILING)
    # perf call graphs through host code need frame pointers, and every
    # caller's frame: a tail call (RTC::ProcessWithInterrupt ends in a jump
    # to RTC::UpdateInterrupts) drops it from frame-pointer and LBR chains
    # alike, and its callee's time lands on the caller's caller.
    add_compile_options(-fno-omit-frame-pointer -fno-optimize-sibling-calls)
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

# What the binary was built from (--build-info, and every profiling trace).
# One header per configuration: a multi-config generator builds Debug and
# Release from one tree, and CMAKE_BUILD_TYPE names neither.
set(OP1_BUILD_INFO_DIR "${CMAKE_BINARY_DIR}/generated/$<CONFIG>")
set(OP1_BUILD_INFO_H "${OP1_BUILD_INFO_DIR}/op1_build_info.h")
add_custom_target(op1-build-info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${OP1_BUILD_INFO_H}
            -DBUILD_TYPE=$<CONFIG> -DPROFILING=${ENABLE_PROFILING}
            "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            -P ${CMAKE_SOURCE_DIR}/cmake/BuildInfo.cmake
    BYPRODUCTS ${OP1_BUILD_INFO_H}
    COMMENT "Recording the build's revision")

file(GLOB_RECURSE SOURCES gui/*.cpp)
add_executable(op1emu ${SOURCES})
add_dependencies(op1emu op1-build-info)
target_include_directories(op1emu PRIVATE ${OP1_BUILD_INFO_DIR})
# First, so that its malloc and free are the ones every library calls.
if(OP1_MIMALLOC)
    target_link_libraries(op1emu PRIVATE mimalloc-static)
endif()
target_link_libraries(op1emu PRIVATE emulator glfw OpenGL::GL nlohmann_json::nlohmann_json uvw ext Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(op1emu PRIVATE ${stb_SOURCE_DIR} ${miniaudio_SOURCE_DIR})

option(OP1_BUILD_HEADLESS_TESTS "Build headless frontend regressions" OFF)
if(OP1_BUILD_HEADLESS_TESTS)
    enable_testing()
    add_executable(op1-ldr-parser-test tests/ldr_parser_test.cpp src/loader/ldr.cpp)
    target_include_directories(op1-ldr-parser-test PRIVATE src)
    add_test(NAME op1-ldr-parser-test COMMAND op1-ldr-parser-test)
    add_executable(op1-rtc-interrupt-test tests/rtc_interrupt_test.cpp)
    target_link_libraries(op1-rtc-interrupt-test PRIVATE emulator)
    add_test(NAME op1-rtc-interrupt-test COMMAND op1-rtc-interrupt-test)
    add_executable(op1-device-service-test tests/device_service_test.cpp)
    target_link_libraries(op1-device-service-test PRIVATE emulator)
    add_test(NAME op1-device-service-test COMMAND op1-device-service-test)
    add_executable(op1-gptimer-test tests/gptimer_test.cpp)
    target_link_libraries(op1-gptimer-test PRIVATE emulator)
    add_test(NAME op1-gptimer-test COMMAND op1-gptimer-test)
    add_executable(op1-cpu-events-test tests/cpu_events_test.cpp)
    target_link_libraries(op1-cpu-events-test PRIVATE emulator bfin-core Threads::Threads)
    add_test(NAME op1-cpu-events-test COMMAND op1-cpu-events-test)
    add_executable(op1-cpu-chain-test tests/cpu_chain_test.cpp)
    target_link_libraries(op1-cpu-chain-test PRIVATE emulator bfin-core Threads::Threads)
    add_test(NAME op1-cpu-chain-test COMMAND op1-cpu-chain-test)
    add_executable(op1-headless-test tests/headless_test.cpp gui/headless.cpp)
    target_include_directories(op1-headless-test PRIVATE gui)
    target_link_libraries(op1-headless-test PRIVATE emulator nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME op1-headless-test COMMAND op1-headless-test)
    set_tests_properties(op1-headless-test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(op1-data-fault-test tests/data_fault_test.cpp)
    target_link_libraries(op1-data-fault-test PRIVATE emulator bfin-core)
    add_test(NAME op1-data-fault-test COMMAND op1-data-fault-test)
    add_executable(op1-audio-ring-test tests/audio_ring_test.cpp)
    target_include_directories(op1-audio-ring-test PRIVATE gui ${miniaudio_SOURCE_DIR})
    target_link_libraries(op1-audio-ring-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME op1-audio-ring-test COMMAND op1-audio-ring-test)
    add_executable(op1-audio-pcm-test tests/audio_pcm_test.cpp)
    target_include_directories(op1-audio-pcm-test PRIVATE gui)
    add_test(NAME op1-audio-pcm-test COMMAND op1-audio-pcm-test)
    add_executable(op1-direct-pages-test tests/direct_pages_test.cpp)
    target_link_libraries(op1-direct-pages-test PRIVATE emulator)
    add_test(NAME op1-direct-pages-test COMMAND op1-direct-pages-test)
    add_executable(op1-time-source-test tests/time_source_test.cpp)
    target_include_directories(op1-time-source-test PRIVATE src)
    add_test(NAME op1-time-source-test COMMAND op1-time-source-test)
    add_executable(op1-realtime-pacer-test tests/realtime_pacer_test.cpp)
    target_include_directories(op1-realtime-pacer-test PRIVATE src)
    add_test(NAME op1-realtime-pacer-test COMMAND op1-realtime-pacer-test)
    add_executable(op1-nfc-ecc-test tests/nfc_ecc_test.cpp)
    target_link_libraries(op1-nfc-ecc-test PRIVATE emulator)
    add_test(NAME op1-nfc-ecc-test COMMAND op1-nfc-ecc-test)
    add_executable(op1-register-test tests/register_test.cpp)
    target_link_libraries(op1-register-test PRIVATE emulator)
    add_test(NAME op1-register-test COMMAND op1-register-test)
    if(ENABLE_PROFILING)
        add_executable(op1-mark-set-test tests/mark_set_test.cpp src/profiling/mark_set.cpp)
        target_include_directories(op1-mark-set-test PRIVATE src)
        add_test(NAME op1-mark-set-test COMMAND op1-mark-set-test)
        add_executable(op1-service-stats-test tests/service_stats_test.cpp)
        target_link_libraries(op1-service-stats-test PRIVATE emulator bfin-core Threads::Threads)
        add_test(NAME op1-service-stats-test COMMAND op1-service-stats-test)
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME op1-ldr-smoke
            COMMAND ${Python3_EXECUTABLE} tests/ldr_smoke.py $<TARGET_FILE:op1emu> $<TARGET_FILE:ldrdump>
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        if(ENABLE_PROFILING)
            add_test(NAME op1-profile-smoke
                COMMAND ${Python3_EXECUTABLE} tests/profile_smoke.py $<TARGET_FILE:op1emu>
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endif()
endif()

option(OP1_BUILD_DMA_TESTS "Build firmware-free DMA regressions" OFF)
if(OP1_BUILD_DMA_TESTS)
    enable_testing()
    add_executable(op1-dma-descriptor-test tests/dma_descriptor_test.cpp)
    target_link_libraries(op1-dma-descriptor-test PRIVATE emulator)
    add_test(NAME op1-dma-descriptor-test COMMAND op1-dma-descriptor-test)
endif()
