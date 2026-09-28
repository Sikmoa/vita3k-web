add_library(
	nids
	STATIC
	include/nids/functions.h
	include/nids/nids.inc
	include/nids/known_nids.inc
	include/nids/nid_aliases.inc
	include/nids/types.h
	src/nids.cpp
)

target_include_directories(nids PUBLIC include)

if(NOT ANDROID)
	add_executable(nids-tests tests/nids_lookup_tests.cpp)
	target_link_libraries(nids-tests PRIVATE nids)
	add_test(NAME nids COMMAND nids-tests)
endif()
