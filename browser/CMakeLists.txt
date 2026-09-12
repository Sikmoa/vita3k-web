cmake_minimum_required(VERSION 3.22)

# M1 deliberately starts as a host-independent lifecycle probe. The full
# emulator graph is added incrementally after each browser-facing boundary is
# tested; native Vita3K targets remain in the normal CMake path above.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vita3k_web
    src/main.cpp
)

target_compile_definitions(vita3k_web PRIVATE VITA3K_WEB=1)

target_link_options(vita3k_web PRIVATE
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_NAME=Vita3KWeb"
    "SHELL:-sENVIRONMENT=web,worker"
    "SHELL:-sEXIT_RUNTIME=1"
    "SHELL:-sNO_EXIT_RUNTIME=0"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sINITIAL_MEMORY=67108864"
    "SHELL:-sSTACK_SIZE=1048576"
)

set_target_properties(vita3k_web PROPERTIES
    OUTPUT_NAME "vita3k_web"
    SUFFIX ".js"
)

add_custom_command(TARGET vita3k_web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
        "${CMAKE_BINARY_DIR}/dist/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/web/worker.js"
        "${CMAKE_BINARY_DIR}/dist/worker.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:vita3k_web>"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vita3k_web.wasm"
        "${CMAKE_BINARY_DIR}/dist/vita3k_web.wasm"
    COMMENT "Staging Vita3K browser bootstrap"
)
