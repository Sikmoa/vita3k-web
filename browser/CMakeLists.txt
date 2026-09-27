cmake_minimum_required(VERSION 3.22)

# M1 deliberately starts as a host-independent lifecycle probe. The full
# emulator graph is added incrementally after each browser-facing boundary is
# tested; native Vita3K targets remain in the normal CMake path above.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_memory.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_core.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_hle.cmake)

# Experimental M14 tests only; the existing application keeps InterpreterCPU.
option(VITA3K_WEB_JIT_TESTS "Build Dynarmic-IR to Wasm JIT prototype tests" OFF)
if(VITA3K_WEB_JIT_TESTS)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "VITA3K_WEB_JIT_TESTS requires Emscripten")
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/runtime_wasmjit.cmake)
endif()

# Genuine VitaSDK fixtures: the display fixture (CPU-rendered animated
# framebuffer) and the message-dialog fixture.
option(VITA3K_WEB_DISPLAY_FIXTURE "Build the genuine VitaSDK display and message-dialog fixtures" ON)
if(VITA3K_WEB_DISPLAY_FIXTURE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests/vita_display_fixture)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests/vita_msg_dialog_fixture)
    # M14c benchmark harness: Node bench modules/run targets over the short
    # fixture. Included before the -O2 add_compile_options line below so the
    # interpreter and JIT bench modules share one effective flag context
    # (the same one the existing vita3k_web_bench target gets).
    include(${CMAKE_CURRENT_SOURCE_DIR}/tests/bench.cmake)
endif()

# Node-runnable interpreter benchmark over the genuine exit-42 fixture. It is
# deliberately built WITHOUT ASYNCIFY so the numbers measure pure interpreter
# throughput; the browser target itself needs ASYNCIFY for cooperative waits.
if(EMSCRIPTEN)
    add_executable(vita3k_web_bench
        src/vita_runtime.cpp
        src/vita_display_bridge.cpp
        tests/vita_bench_main.cpp
    )
    target_link_libraries(vita3k_web_bench PRIVATE vita3k_web_runtime_hle)
    # Retail-app launch path (VFS multi-module boot for staged content).
    # Shared by the browser module, the bench harnesses and the JIT bench
    # variants below; backend selection stays a compile definition.
    set(VITA3K_WEB_APP_SOURCES
        src/vita_app.cpp
        src/vita_self_decrypt.cpp
    )
    add_executable(vita3k_web_app_bench
        src/vita_runtime.cpp
        src/vita_display_bridge.cpp
        ${VITA3K_WEB_APP_SOURCES}
        tests/vita_app_bench_main.cpp
    )
    target_link_libraries(vita3k_web_app_bench PRIVATE vita3k_web_runtime_hle)
    if(TARGET vita3k_wasm_jit)
        target_compile_definitions(vita3k_web_app_bench PRIVATE VITA3K_WEB=1 VITA3K_USE_WASM_JIT=1)
        target_link_libraries(vita3k_web_app_bench PRIVATE vita3k_guest_thread_runtime)
    else()
        message(FATAL_ERROR "vita3k_web_app_bench requires VITA3K_WEB_JIT_TESTS=ON")
    endif()
    add_executable(vita3k_guest_thread_tests
        tests/guest_thread_lifecycle_test.cpp
        src/vita_self_decrypt.cpp
    )
    target_link_libraries(vita3k_guest_thread_tests PRIVATE vita3k_guest_thread_runtime)
    target_link_options(vita3k_guest_thread_tests PRIVATE
        ${VITA3K_WEB_GROWTH_LINK_OPTION} "SHELL:-sSTACK_SIZE=1048576")
    target_link_options(vita3k_web_app_bench PRIVATE
        ${VITA3K_WEB_GROWTH_LINK_OPTION}
        "SHELL:-sSTACK_SIZE=1048576"
        "SHELL:-sNODERAWFS=1"
    )
    target_link_options(vita3k_web_bench PRIVATE
        ${VITA3K_WEB_GROWTH_LINK_OPTION}
        "SHELL:-sSTACK_SIZE=1048576"
        "SHELL:-sNODERAWFS=1"
    )
endif()

# ARM guest code is built with the VitaSDK toolchain, as the fixtures are. A
# plain `clang` is not enough: emsdk's clang (first on PATH in an Emscripten
# environment) has no ARM backend.
set(VITASDK "$ENV{VITASDK}" CACHE PATH "VitaSDK root (containing bin/arm-vita-eabi-gcc)")
set(VITA3K_ARM_GCC "${VITASDK}/bin/arm-vita-eabi-gcc")
if(NOT EXISTS "${VITA3K_ARM_GCC}")
    message(FATAL_ERROR "Set VITASDK to a VitaSDK installation; missing ${VITA3K_ARM_GCC}")
endif()

# The browser runtime is an emulated machine: debug (-g, no optimization) Wasm
# runs the interpreter ~100x too slow for frame-paced guest loops. Default the
# browser target to optimized codegen while keeping debug info available via
# a separate build directory configured with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND EMSCRIPTEN)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options($<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:MinSizeRel>:-Os>)

set(VITA3K_GUEST_PROBE_ELF "${CMAKE_CURRENT_BINARY_DIR}/guest_probe.elf")
add_custom_command(
    OUTPUT "${VITA3K_GUEST_PROBE_ELF}"
    COMMAND "${VITA3K_ARM_GCC}"
        -mthumb
        -nostdlib
        "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.ld"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.S"
        -o "${VITA3K_GUEST_PROBE_ELF}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_probe/guest.ld"
        "${VITA3K_ARM_GCC}"
    COMMENT "Building ARM guest ELF probe"
    VERBATIM
)
add_custom_target(vita3k_guest_probe_elf ALL DEPENDS "${VITA3K_GUEST_PROBE_ELF}")

add_executable(vita3k_web
    src/main.cpp
    src/vita_runtime.cpp
    src/vita_display_bridge.cpp
    ${VITA3K_WEB_APP_SOURCES}
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
)

target_compile_definitions(vita3k_web PRIVATE VITA3K_WEB=1)
target_link_libraries(vita3k_web PRIVATE vita3k_web_runtime_hle)

add_executable(vita3k_web_memory_tests
    src/memory.cpp
    tests/memory_tests.cpp
)

add_executable(vita3k_web_interpreter_tests
    src/memory.cpp
    src/interpreter.cpp
    tests/interpreter_tests.cpp
)

add_executable(vita3k_web_guest_tests
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
    tests/guest_tests.cpp
)

add_executable(vita3k_web_generated_elf_tests
    src/memory.cpp
    src/interpreter.cpp
    src/guest.cpp
    tests/guest_probe/guest_probe_tests.cpp
)
add_dependencies(vita3k_web_generated_elf_tests vita3k_guest_probe_elf)
if(EMSCRIPTEN)
    target_link_options(vita3k_web_generated_elf_tests PRIVATE -sNODERAWFS=1)
endif()

if(NOT EMSCRIPTEN)
    enable_testing()
    add_test(NAME vita3k_web_memory COMMAND vita3k_web_memory_tests)
    add_test(NAME vita3k_web_interpreter COMMAND vita3k_web_interpreter_tests)
    add_test(NAME vita3k_web_guest COMMAND vita3k_web_guest_tests)
    add_test(NAME vita3k_web_generated_elf COMMAND vita3k_web_generated_elf_tests "${VITA3K_GUEST_PROBE_ELF}")
endif()

target_link_options(vita3k_web PRIVATE
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_ES6=1"
    "SHELL:-sEXPORT_NAME=Vita3KWeb"
    "SHELL:-sENVIRONMENT=web,worker"
    "SHELL:-sEXIT_RUNTIME=0"
    "SHELL:-sNO_EXIT_RUNTIME=1"
    ${VITA3K_WEB_MEMORY_LINK_OPTIONS}
    "SHELL:-sSTACK_SIZE=1048576"
    "SHELL:-sASYNCIFY"
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_vita3k_web_run_vita','_vita3k_web_last_run_instructions','_vita3k_web_set_app_paths','_vita3k_web_set_license_key','_vita3k_web_run_app']"
)

set_target_properties(vita3k_web PROPERTIES
    OUTPUT_NAME "vita3k_web"
    SUFFIX ".js"
)

# Deployable web root, built by vita3k_web_dist. Every file in it is the
# output of a rule that depends on its source (web asset, module link,
# fixture), so a deleted or stale dist is rebuilt by the next build.
set(VITA3K_WEB_DIST "${CMAKE_BINARY_DIR}/dist")
set(_dist_outputs "")
# vita3k_web_dist_copy(<path in dist> <source file>)
function(vita3k_web_dist_copy destination source)
    set(_dst "${VITA3K_WEB_DIST}/${destination}")
    get_filename_component(_dir "${_dst}" DIRECTORY)
    add_custom_command(
        OUTPUT "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${_dst}"
        DEPENDS "${source}"
        COMMENT "Staging dist/${destination}"
        VERBATIM
    )
    set(_dist_outputs ${_dist_outputs} "${_dst}" PARENT_SCOPE)
endfunction()
# vita3k_web_dist_module(<module target>): its .js and the .wasm linked with it.
# The directory is the one worker.js loads this build's memory model from.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/web/worker.js" _module_directories
    REGEX "^const moduleDirectories = ")
if(NOT _module_directories MATCHES "w64: '([^']*)', w32: '([^']*)'")
    message(FATAL_ERROR "web/worker.js: no `const moduleDirectories = { w64: '...', w32: '...' }` line")
endif()
if(VITA3K_WEB_MEMORY64)
    set(VITA3K_WEB_MODULE_DIR "${VITA3K_WEB_DIST}/${CMAKE_MATCH_1}")
else()
    set(VITA3K_WEB_MODULE_DIR "${VITA3K_WEB_DIST}/${CMAKE_MATCH_2}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/worker.js")
function(vita3k_web_dist_module target)
    set(_dir "${VITA3K_WEB_MODULE_DIR}")
    add_custom_command(
        OUTPUT "${_dir}/${target}.js" "${_dir}/${target}.wasm"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:${target}>/${target}.js" "$<TARGET_FILE_DIR:${target}>/${target}.wasm" "${_dir}"
        DEPENDS ${target}
        COMMENT "Staging ${target} into dist"
        VERBATIM
    )
    set(_dist_outputs ${_dist_outputs} "${_dir}/${target}.js" "${_dir}/${target}.wasm" PARENT_SCOPE)
endfunction()

# dist/shaders/ holds what web_gxm_init (gxm_webgpu_bridge.cpp) loads: the
# GXP compiler, built from browser/shaders as its own wasm32 project, and Naga
# plus the WASI shim, installed by `npm ci` from the pinned
# browser/shaders/package-lock.json. That install is the one input the
# configure step fetches: it needs npm and the npm registry (or an npm cache
# holding the packages).
include(ExternalProject)
set(_gxp_compiler_dir "${CMAKE_CURRENT_BINARY_DIR}/gxp_compiler")
ExternalProject_Add(vita3k_gxp_compiler
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    BINARY_DIR "${_gxp_compiler_dir}"
    CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON # the inner build tracks the translator sources itself
    BUILD_BYPRODUCTS "${_gxp_compiler_dir}/gxp_compiler.mjs" "${_gxp_compiler_dir}/gxp_compiler.wasm"
)
find_program(VITA3K_NPM npm)
if(NOT VITA3K_NPM)
    message(FATAL_ERROR "npm not found: it installs Naga and the WASI shim for dist/shaders "
        "(browser/shaders/package-lock.json)")
endif()
# Installed at configure time, like a FetchContent dependency, so every file
# the shim ships is known here and gets its own staging rule; a changed
# lockfile reconfigures and reinstalls.
set(_shader_deps "${CMAKE_CURRENT_BINARY_DIR}/shader_deps")
set(_naga "${_shader_deps}/node_modules/naga-wasi-cli")
set(_wasi_shim "${_shader_deps}/node_modules/@bjorn3/browser_wasi_shim")
set(_shader_lock "${CMAKE_CURRENT_SOURCE_DIR}/shaders/package-lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/package.json" "${_shader_lock}")
file(SHA256 "${_shader_lock}" _shader_lock_hash)
set(_shader_installed "")
if(EXISTS "${_shader_deps}/installed.sha256" AND EXISTS "${_naga}/wasi/naga.wasm" AND EXISTS "${_wasi_shim}/dist/index.js")
    file(READ "${_shader_deps}/installed.sha256" _shader_installed)
endif()
if(NOT _shader_installed STREQUAL _shader_lock_hash)
    file(REMOVE_RECURSE "${_shader_deps}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shaders/package.json" "${_shader_lock}" DESTINATION "${_shader_deps}")
    message(STATUS "Installing Naga and the WASI shim (npm ci in ${_shader_deps})")
    execute_process(COMMAND "${VITA3K_NPM}" ci --ignore-scripts --no-audit --no-fund
        WORKING_DIRECTORY "${_shader_deps}" RESULT_VARIABLE _npm_result)
    if(NOT _npm_result EQUAL 0)
        message(FATAL_ERROR "npm ci failed (${_npm_result}); it needs the npm registry or an npm cache "
            "holding the packages pinned in ${_shader_lock}")
    endif()
    # npm keeps the packages' 1985 timestamps: date them now so staged copies
    # of an older install count as out of date.
    file(GLOB_RECURSE _installed_files "${_shader_deps}/node_modules/*")
    file(TOUCH_NOCREATE ${_installed_files})
    file(WRITE "${_shader_deps}/installed.sha256" "${_shader_lock_hash}")
endif()
file(GLOB _wasi_shim_modules "${_wasi_shim}/dist/*.js")
foreach(_module IN LISTS _wasi_shim_modules)
    get_filename_component(_name "${_module}" NAME)
    vita3k_web_dist_copy("shaders/wasi/${_name}" "${_module}")
endforeach()
vita3k_web_dist_copy(shaders/gxp_compiler.mjs "${_gxp_compiler_dir}/gxp_compiler.mjs")
vita3k_web_dist_copy(shaders/gxp_compiler.wasm "${_gxp_compiler_dir}/gxp_compiler.wasm")
vita3k_web_dist_copy(shaders/naga.wasm "${_naga}/wasi/naga.wasm")
vita3k_web_dist_copy(shaders/naga-LICENSE-MIT "${_naga}/LICENSE-MIT")
vita3k_web_dist_copy(shaders/naga-LICENSE-APACHE "${_naga}/LICENSE-APACHE")
vita3k_web_dist_copy(shaders/wasi/LICENSE-MIT "${_wasi_shim}/LICENSE-MIT")
vita3k_web_dist_copy(shaders/wasi/LICENSE-APACHE "${_wasi_shim}/LICENSE-APACHE")

# Every file in web/ is deployed; check_web_imports.cmake fails the build when
# a script loaded by relative URL (from web/ or an EM_JS import) is not there.
file(GLOB _web_assets LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
file(GLOB _em_js_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(_web_imports_stamp "${CMAKE_CURRENT_BINARY_DIR}/web_imports.stamp")
# Rewritten only when a glob changes, so removing a file also reruns the check.
set(_web_imports_inputs "${CMAKE_CURRENT_BINARY_DIR}/web_imports.inputs")
file(CONFIGURE OUTPUT "${_web_imports_inputs}" CONTENT "${_web_assets}\n${_em_js_sources}\n")
add_custom_command(
    OUTPUT "${_web_imports_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web"
        "-DSOURCES=${_em_js_sources}"
        "-DSTAMP=${_web_imports_stamp}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_web_imports.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/check_web_imports.cmake" "${_web_imports_inputs}"
        ${_web_assets} ${_em_js_sources}
    COMMENT "Checking that web/ holds every script the browser loads"
    VERBATIM
)
list(APPEND _dist_outputs "${_web_imports_stamp}")
foreach(_asset IN LISTS _web_assets)
    get_filename_component(_name "${_asset}" NAME)
    vita3k_web_dist_copy("${_name}" "${_asset}")
endforeach()
vita3k_web_dist_copy(guest.elf "${VITA3K_GUEST_PROBE_ELF}")
if(TARGET vita3k_vita_display_fixture)
    vita3k_web_dist_copy(display-eboot.bin "${CMAKE_CURRENT_BINARY_DIR}/tests/vita_display_fixture/eboot.bin")
endif()
vita3k_web_dist_module(vita3k_web)
if(TARGET vita3k_web_jit)
    vita3k_web_dist_module(vita3k_web_jit)
endif()
add_custom_target(vita3k_web_dist ALL DEPENDS ${_dist_outputs})
add_dependencies(vita3k_web_dist vita3k_gxp_compiler)
if(TARGET vita3k_vita_display_fixture)
    add_dependencies(vita3k_web_dist vita3k_vita_display_fixture)
endif()

vita3k_web_finalize_memory64("${CMAKE_CURRENT_SOURCE_DIR}")
