# The browser's op1emu (web/README.md): one module for a Web Worker and for
# Node (the tests), built with emcmake as bcore's WebAssembly build is
# (ext/bcore/tools/wasm/README.md).

add_executable(op1emu-web src/op1_web.cpp src/gate.cpp ${CMAKE_SOURCE_DIR}/src/profiling/mark_set.cpp)
set_target_properties(op1emu-web PROPERTIES SUFFIX ".mjs")
target_include_directories(op1emu-web PRIVATE src ${CMAKE_SOURCE_DIR}/gui ${CMAKE_SOURCE_DIR}/src/profiling)
target_link_libraries(op1emu-web PRIVATE emulator bfin-core nlohmann_json::nlohmann_json)
# bcore's (its CMakeLists.txt): -O1, as wasm-opt's higher levels cost a
# minute a link and start slower in V8; memory and the function table grow,
# the table with every block; it starts with 128 MiB, for bcore's stencils
# (43 MiB of data). Then a module that a Worker or Node imports,
# with no main: the host calls op1_start and op1_step.
target_link_options(op1emu-web PRIVATE
    -fwasm-exceptions -O1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=4GB -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=8MB
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createOp1 -sENVIRONMENT=worker,node --no-entry
    -lnodefs.js -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,HEAPU16,HEAPF32,UTF8ToString,stringToNewUTF8
    -sEXPORTED_FUNCTIONS=_malloc,_free
    # LLVM in a module of its own, loaded only if a block needs it
    # (tools/split-module.mjs, run after the link): the link keeps the names
    # it goes by in op1emu-web.wasm.orig. src/split-module-pre.js: the glue
    # finds the deferred module where the Worker instantiates the module too.
    -sSPLIT_MODULE --profiling-funcs -Wno-experimental
    "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/src/split-module-pre.js")
set_property(TARGET op1emu-web APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/split-module-pre.js")
find_program(OP1_WASM_SPLIT wasm-split HINTS "${EMSCRIPTEN_ROOT_PATH}/../bin" REQUIRED)
add_custom_command(TARGET op1emu-web POST_BUILD
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${CMAKE_CURRENT_SOURCE_DIR}/tools/split-module.mjs" "$<TARGET_FILE_DIR:op1emu-web>"
            "${OP1_WASM_SPLIT}"
    COMMENT "Moving LLVM to op1emu-web.deferred.wasm"
    VERBATIM)
