set(SOURCE_LIST
	include/kernel/state.h
	include/kernel/types.h
	include/kernel/thread/thread_data_queue.h
	include/kernel/thread/thread_state.h
	include/kernel/sync_primitives.h
	include/kernel/relocation.h
	include/kernel/object_store.h
	include/kernel/debugger.h
	include/kernel/load_self.h
	include/kernel/callback.h
	src/kernel.cpp
	src/thread.cpp
	src/debugger.cpp
	src/load_self.cpp
	src/sync_primitives.cpp
	src/relocation.cpp
	src/callback.cpp
)

add_library(
	kernel
	STATIC
	${SOURCE_LIST}
)

target_include_directories(kernel PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../emuenv/include)
target_link_libraries(kernel PUBLIC rtc cpu mem util nids)
target_link_libraries(kernel PRIVATE SDL3::SDL3 miniz vita-toolchain)
if(TRACY_ENABLE_ON_CORE_COMPONENTS)
	target_link_libraries(kernel PRIVATE tracy)
endif()
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

if(VITA3K_INTERPRETER_CPU AND NOT ANDROID)
    add_executable(vita3k_interpreter_runtime_tests
        ../cpu/tests/interpreter_runtime_tests.cpp
    )
    target_link_libraries(vita3k_interpreter_runtime_tests PRIVATE kernel)
    add_test(NAME vita3k_interpreter_runtime COMMAND vita3k_interpreter_runtime_tests)

    add_executable(vita3k_interpreter_instruction_tests
        ../cpu/tests/interpreter_instruction_tests.cpp
    )
    target_link_libraries(vita3k_interpreter_instruction_tests PRIVATE cpu mem util)
    add_test(NAME vita3k_interpreter_instruction COMMAND vita3k_interpreter_instruction_tests)
endif()