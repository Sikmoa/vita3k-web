set(SOURCE_LIST
include/cpu/state.h
include/cpu/common.h
include/cpu/functions.h
include/cpu/impl/interpreter_cpu.h
include/cpu/impl/interface.h
include/cpu/disasm/functions.h
include/cpu/disasm/state.h

src/disasm.cpp
src/cpu.cpp
)

if(VITA3K_INTERPRETER_CPU)
    list(APPEND SOURCE_LIST include/cpu/impl/interpreter_cpu.h src/interpreter_cpu.cpp)
endif()

if(NOT VITA3K_INTERPRETER_CPU)
    list(APPEND SOURCE_LIST include/cpu/impl/dynarmic_cpu.h src/dynarmic_cpu.cpp)
endif()

add_library(
cpu
STATIC
${SOURCE_LIST}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

target_include_directories(cpu PUBLIC include)
target_link_libraries(cpu PUBLIC mem util)
target_link_libraries(cpu PRIVATE capstone merry::mcl)
if(NOT VITA3K_INTERPRETER_CPU)
    target_link_libraries(cpu PRIVATE dynarmic)
endif()
if(VITA3K_INTERPRETER_CPU)
    target_compile_definitions(cpu PRIVATE VITA3K_INTERPRETER_CPU=1)
endif()
