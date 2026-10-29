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
    -sEXPORTED_FUNCTIONS=_malloc,_free)
