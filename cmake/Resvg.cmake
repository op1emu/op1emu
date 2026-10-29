# resvg's C API builds a static libresvg, avoiding runtime library packaging.
find_program(OP1_CARGO cargo REQUIRED)
find_program(OP1_RUSTC rustc REQUIRED)
execute_process(COMMAND ${OP1_RUSTC} --version OUTPUT_VARIABLE rust_version
                OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT rust_version MATCHES "rustc ([0-9]+\\.[0-9]+\\.[0-9]+)" OR CMAKE_MATCH_1 VERSION_LESS 1.85)
    message(FATAL_ERROR "SVG support requires Rust >= 1.85; found ${rust_version}")
endif()
if(CMAKE_CROSSCOMPILING OR (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";"))
    message(FATAL_ERROR "resvg integration currently supports native, single-architecture builds")
endif()
FetchContent_Declare(resvg
    GIT_REPOSITORY https://github.com/linebender/resvg.git
    GIT_TAG 66ee0dc7d787d7e0b0ab3085b7849f78838cf2ba # v0.49.0
)
FetchContent_MakeAvailable(resvg)
set(resvg_target_dir "${CMAKE_BINARY_DIR}/resvg-target")
set(resvg_library "${resvg_target_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}resvg${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_custom_command(OUTPUT "${resvg_library}"
    COMMAND ${OP1_CARGO} build --release --locked -p resvg-capi
            --manifest-path "${resvg_SOURCE_DIR}/Cargo.toml" --target-dir "${resvg_target_dir}"
    DEPENDS "${resvg_SOURCE_DIR}/Cargo.lock" "${resvg_SOURCE_DIR}/crates/c-api/lib.rs"
    COMMENT "Building resvg-capi 0.49.0 (locked dependencies)" VERBATIM)
add_custom_target(op1-resvg-build DEPENDS "${resvg_library}")
add_library(op1-resvg STATIC IMPORTED GLOBAL)
set_target_properties(op1-resvg PROPERTIES IMPORTED_LOCATION "${resvg_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${resvg_SOURCE_DIR}/crates/c-api")
add_dependencies(op1-resvg op1-resvg-build)
find_package(Threads REQUIRED)
target_link_libraries(op1-resvg INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
    find_library(OP1_COREFOUNDATION CoreFoundation REQUIRED)
    find_library(OP1_CORETEXT CoreText REQUIRED)
    find_library(OP1_COREGRAPHICS CoreGraphics REQUIRED)
    target_link_libraries(op1-resvg INTERFACE ${OP1_COREFOUNDATION} ${OP1_CORETEXT} ${OP1_COREGRAPHICS} "-lresolv")
elseif(UNIX)
    target_link_libraries(op1-resvg INTERFACE m)
endif()
