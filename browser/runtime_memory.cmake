# UNBUILT / UNVALIDATED: see MEMORY64.md for the required toolchain contract.
# Included before ALL browser/dependency targets, including object libraries.
include_guard(DIRECTORY)
if(VITA3K_WEB_MEMORY64)
    if(NOT EMSCRIPTEN OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Memory64 requires a fresh Emscripten wasm64 build directory")
    endif()
    add_compile_definitions(VITA3K_WEB_MEMORY64=1)
    # Emscripten 3.1.69's supported wasm64 switch. Newer toolchains may move
    # back to -m64 once their final-link support is available; this setting is
    # intentionally localized here rather than applied to native targets.
    add_link_options(-sMEMORY64=1)
    # Runtime bytes <4 GiB, fixed guest bytes [4 GiB,8 GiB). Physical memory
    # behavior is unspecified here. The bounded morecore object is mandatory:
    # INITIAL_MEMORY alone does NOT keep malloc out of the guest window.
    set(VITA3K_WEB_MEMORY_LINK_OPTIONS
        -sINITIAL_MEMORY=8589934592 -sMAXIMUM_MEMORY=8589934592
        -sALLOW_MEMORY_GROWTH=0 -sMALLOC=dlmalloc -sABORTING_MALLOC=0
        -sWASM_BIGINT=1)
    add_library(vita3k_web_memory64_heap OBJECT src/memory64_heap.cpp)
    target_include_directories(vita3k_web_memory64_heap PRIVATE ../vita3k/mem/include)
    set(VITA3K_WEB_GROWTH_LINK_OPTION -sALLOW_MEMORY_GROWTH=0)
    set(VITA3K_WEB_INITIAL_MEMORY_LINK_OPTION -sINITIAL_MEMORY=8589934592)
else()
    set(VITA3K_WEB_GROWTH_LINK_OPTION -sALLOW_MEMORY_GROWTH=1)
    set(VITA3K_WEB_INITIAL_MEMORY_LINK_OPTION -sINITIAL_MEMORY=67108864)
    set(VITA3K_WEB_MEMORY_LINK_OPTIONS
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864)
endif()

# Call after constructing the graph. Add the object to final executables only:
# adding it to every archive could produce duplicate morecore definitions.
function(vita3k_web_finalize_memory64 directory)
    if(NOT VITA3K_WEB_MEMORY64)
        return()
    endif()
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)
        if(_type STREQUAL "EXECUTABLE")
            if(_target STREQUAL "vita3k_threads_probe")
                # This target uses pthreads even in the single-Worker build.
                # Compile the bounded heap with the probe's own atomics flags.
                target_sources("${_target}" PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/memory64_heap.cpp")
                target_include_directories("${_target}" PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../vita3k/mem/include")
            else()
                target_sources("${_target}" PRIVATE $<TARGET_OBJECTS:vita3k_web_memory64_heap>)
                add_dependencies("${_target}" vita3k_web_memory64_heap)
            endif()
        endif()
    endforeach()
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        vita3k_web_finalize_memory64("${_child}")
    endforeach()
endfunction()

# Applied to all final browser targets and inherited by dependency subdirs.
# Do not add conflicting per-target growth/initial/max-memory settings.
if(VITA3K_WEB_MEMORY64)
    add_link_options(${VITA3K_WEB_MEMORY_LINK_OPTIONS})
endif()

# Threaded build (THREADS.md): configured in its own build directory with
# -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread, because Wasm objects
# built with and without shared-memory atomics cannot be linked together.
if(CMAKE_CXX_FLAGS MATCHES "-pthread")
    set(VITA3K_WEB_THREADS ON)
    add_compile_definitions(VITA3K_WEB_THREADS=1)
    add_link_options(-pthread)
else()
    set(VITA3K_WEB_THREADS OFF)
endif()
# C++ exceptions: native Wasm exceptions where nothing needs Asyncify (the
# threaded build); the fiber build keeps JS-based ones, which Asyncify can
# unwind. Every object in one link must use the same model.
if(VITA3K_WEB_THREADS)
    set(VITA3K_WEB_EXCEPTIONS -fwasm-exceptions)
else()
    set(VITA3K_WEB_EXCEPTIONS -fexceptions)
endif()
