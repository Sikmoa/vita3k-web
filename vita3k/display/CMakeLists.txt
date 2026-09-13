add_library(
	display
	STATIC
	include/display/state.h
	include/display/functions.h
	src/display.cpp
)

target_include_directories(display PUBLIC include)
target_link_libraries(display PUBLIC emuenv kernel)
target_link_libraries(display PRIVATE touch renderer dialog motion)

if(NOT ANDROID)
	add_executable(
		display-tests
		tests/advance_vblank_tests.cpp
		tests/stb_image_impl.c
	)

	target_link_libraries(display-tests PRIVATE display googletest stb)
	# Tracy's profiler performs an invariant-TSC check at static initialization
	# and aborts on hosts without it (virtualized CI). The tracy-linked Debug
	# configuration is exactly TRACY_ENABLE_ON_CORE_COMPONENTS, so register the
	# test only where the binary is tracy-free; the target builds everywhere.
	if(NOT TRACY_ENABLE_ON_CORE_COMPONENTS)
		add_test(NAME display COMMAND display-tests)
	endif()
endif()
