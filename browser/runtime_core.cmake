# Authoritative Vita3K runtime sources, built independently of desktop frontend.
set(VITA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vita3k")
set(EXT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external")
add_library(vita3k_web_host_abi INTERFACE)
target_link_options(vita3k_web_host_abi INTERFACE
    --pre-js "${CMAKE_CURRENT_LIST_DIR}/src/host_abi.js"
    --post-js "${CMAKE_CURRENT_LIST_DIR}/src/memory64_post.js")
set_property(TARGET vita3k_web_host_abi PROPERTY INTERFACE_LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/host_abi.js"
    "${CMAKE_CURRENT_LIST_DIR}/src/memory64_post.js")
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "" FORCE)
add_subdirectory("${EXT_ROOT}/capstone" "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/capstone" EXCLUDE_FROM_ALL)
add_subdirectory("${EXT_ROOT}/fmt" "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/fmt" EXCLUDE_FROM_ALL)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory("${EXT_ROOT}/spdlog" "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/spdlog" EXCLUDE_FROM_ALL)
file(GLOB BOOST_HEADER_DIRS "${EXT_ROOT}/boost/libs/*/include")
file(GLOB BOOST_NUMERIC_HEADER_DIRS "${EXT_ROOT}/boost/libs/numeric/*/include")
file(GLOB BOOST_FILESYSTEM_SOURCES "${EXT_ROOT}/boost/libs/filesystem/src/*.cpp")
add_library(vita3k_web_boost_filesystem STATIC ${BOOST_FILESYSTEM_SOURCES})
target_include_directories(vita3k_web_boost_filesystem PUBLIC "${EXT_ROOT}/boost")
target_compile_definitions(vita3k_web_boost_filesystem PRIVATE BOOST_FILESYSTEM_SOURCE BOOST_FILESYSTEM_STATIC_LINK BOOST_FILESYSTEM_SINGLE_THREADED)
add_library(vita3k_web_runtime_core STATIC
    "${VITA_ROOT}/mem/src/mem.cpp" "${VITA_ROOT}/mem/src/allocator.cpp"
    "${VITA_ROOT}/cpu/src/cpu.cpp" "${VITA_ROOT}/cpu/src/interpreter_cpu.cpp" "${VITA_ROOT}/cpu/src/disasm.cpp"
    "${VITA_ROOT}/kernel/src/kernel.cpp" "${VITA_ROOT}/kernel/src/thread.cpp"
    "${VITA_ROOT}/kernel/src/debugger.cpp" "${VITA_ROOT}/kernel/src/callback.cpp"
    "${VITA_ROOT}/kernel/src/load_self.cpp" "${VITA_ROOT}/kernel/src/relocation.cpp"
    "${VITA_ROOT}/rtc/src/rtc.cpp" "${VITA_ROOT}/nids/src/nids.cpp"
    "${VITA_ROOT}/util/src/arm.cpp" "${VITA_ROOT}/util/src/fs_utils.cpp" "${VITA_ROOT}/util/src/string_utils.cpp"
    "${VITA_ROOT}/patch/src/patch.cpp" "${VITA_ROOT}/patch/src/instructions.cpp" "${VITA_ROOT}/patch/src/util.cpp"
    "${EXT_ROOT}/miniz/miniz.c"
)
target_include_directories(vita3k_web_runtime_core PUBLIC
    "${VITA_ROOT}/mem/include" "${VITA_ROOT}/cpu/include" "${VITA_ROOT}/kernel/include"
    "${VITA_ROOT}/rtc/include" "${VITA_ROOT}/nids/include" "${VITA_ROOT}/util/include" "${VITA_ROOT}/patch/include"
    "${VITA_ROOT}/emuenv/include" "${EXT_ROOT}/sdl/include"
    "${EXT_ROOT}/vita-toolchain/src" "${EXT_ROOT}/miniz"
    "${EXT_ROOT}/boost"
    ${BOOST_HEADER_DIRS} ${BOOST_NUMERIC_HEADER_DIRS}
)
target_compile_definitions(vita3k_web_runtime_core PUBLIC VITA3K_INTERPRETER_CPU=1 SPDLOG_FMT_EXTERNAL)
target_compile_options(vita3k_web_runtime_core PUBLIC -fexceptions)
target_link_options(vita3k_web_runtime_core INTERFACE -fexceptions)
target_link_libraries(vita3k_web_runtime_core PUBLIC fmt::fmt spdlog::spdlog capstone vita3k_web_boost_filesystem vita3k_web_host_abi)
add_executable(vita3k_web_runtime_tests "${VITA_ROOT}/cpu/tests/interpreter_runtime_tests.cpp")
target_link_libraries(vita3k_web_runtime_tests PRIVATE vita3k_web_runtime_core)
target_link_options(vita3k_web_runtime_tests PRIVATE ${VITA3K_WEB_GROWTH_LINK_OPTION} -sSTACK_SIZE=1048576)
add_executable(vita3k_web_patch_tests "${VITA_ROOT}/patch/tests/patch_tests.cpp")
target_link_libraries(vita3k_web_patch_tests PRIVATE vita3k_web_runtime_core)
# Reads the shipped browser/patches directory from the host file system.
target_link_options(vita3k_web_patch_tests PRIVATE ${VITA3K_WEB_GROWTH_LINK_OPTION} -sNODERAWFS=1)
add_executable(vita3k_web_loader_tests "${CMAKE_CURRENT_LIST_DIR}/tests/vita_loader_runtime_tests.cpp")
target_link_libraries(vita3k_web_loader_tests PRIVATE vita3k_web_runtime_core)
# This executable is a Node-only file-input test; browser entrypoints do not use
# NODERAWFS and receive uploaded bytes instead.
target_link_options(vita3k_web_loader_tests PRIVATE ${VITA3K_WEB_GROWTH_LINK_OPTION} -sSTACK_SIZE=1048576 -sNODERAWFS=1)
