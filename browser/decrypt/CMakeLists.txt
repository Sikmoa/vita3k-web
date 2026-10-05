# Game decryption for the browser player: a NoNpDrm dump's PFS layer
# (psvpfstools' psvpfsparser) and its SELFs (Vita3K's decrypt_fself), as the
# module decrypt_worker.js drives (vita3k_decrypt.mjs/.wasm in dist/decrypt/).
#
# The browser build (browser/CMakeLists.txt) builds it as its own wasm32
# project, like the GXP compiler: the worker passes Number pointers, and one
# file at a time never needs more than 4 GiB. Desktop links OpenSSL 3; this
# builds libcrypto from the release tarball with Emscripten (no asm, threads
# or sockets). VITA3K_DECRYPT_OPENSSL_URL may point at a local mirror.
cmake_minimum_required(VERSION 3.22)
project(vita3k_decrypt C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_ext "${_repo}/external")
include(ExternalProject)

set(VITA3K_DECRYPT_OPENSSL_URL
    "https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz" CACHE STRING
    "OpenSSL 3 source tarball (libcrypto for AES, SHA and HMAC)")
set(_openssl_prefix "${CMAKE_CURRENT_BINARY_DIR}/openssl")
set(_openssl_source "${_openssl_prefix}/src/openssl_build")
find_program(VITA3K_PERL perl REQUIRED)
ExternalProject_Add(openssl_build
    URL "${VITA3K_DECRYPT_OPENSSL_URL}"
    URL_HASH SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${_openssl_prefix}"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
        "${VITA3K_PERL}" ./Configure linux-generic32 no-asm no-threads no-shared no-dso no-engine
        no-afalgeng no-async no-sock no-tests no-apps no-docs no-ui-console no-secure-memory -O2
    BUILD_COMMAND make -j8 build_generated COMMAND make -j8 libcrypto.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${_openssl_source}/libcrypto.a"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON
)
add_library(decrypt_openssl INTERFACE)
add_dependencies(decrypt_openssl openssl_build)
target_include_directories(decrypt_openssl INTERFACE "${_openssl_source}/include")
target_link_libraries(decrypt_openssl INTERFACE "${_openssl_source}/libcrypto.a")

file(GLOB _pfs_sources "${_ext}/psvpfstools/psvpfsparser/*.cpp")
file(GLOB _boost_filesystem_sources "${_ext}/boost/libs/filesystem/src/*.cpp")
add_executable(vita3k_decrypt
    decrypt.cpp
    util_stubs.cpp
    ${_pfs_sources}
    "${_ext}/psvpfstools/libzrif/src/keyflate.c"
    "${_ext}/psvpfstools/libzrif/src/licdec.cpp"
    "${_ext}/psvpfstools/libb64/src/cencode.c"
    "${_ext}/psvpfstools/libb64/src/cdecode.c"
    "${_repo}/vita3k/packages/src/sce_utils.cpp"
    "${_ext}/miniz/miniz.c"
    "${_ext}/fmt/src/format.cc"
    ${_boost_filesystem_sources}
)
file(GLOB _fat16_sources "${_ext}/libfat16/src/*.c" "${_ext}/libfat16/src/*.cpp")
target_sources(vita3k_decrypt PRIVATE ${_fat16_sources})
target_include_directories(vita3k_decrypt PRIVATE
    "${_ext}/psvpfstools/psvpfsparser" "${_ext}/psvpfstools/libzrif/include" "${_ext}/psvpfstools/libb64/include"
    "${_repo}/vita3k/packages/include" "${_repo}/vita3k/util/include" "${_ext}/vita-toolchain/src"
    "${_ext}/libfat16/include" "${_ext}/miniz" "${_ext}/fmt/include" "${_ext}/spdlog/include" "${_ext}/boost")
target_compile_definitions(vita3k_decrypt PRIVATE SPDLOG_FMT_EXTERNAL SPDLOG_NO_THREAD_ID SPDLOG_NO_TLS
    BOOST_FILESYSTEM_SOURCE BOOST_FILESYSTEM_STATIC_LINK BOOST_FILESYSTEM_SINGLE_THREADED)
target_link_libraries(vita3k_decrypt PRIVATE decrypt_openssl)
target_compile_options(vita3k_decrypt PRIVATE -O2 -fexceptions -sUSE_ZLIB=1 -Wno-deprecated-declarations)
if(EMSCRIPTEN)
    target_link_options(vita3k_decrypt PRIVATE
        -O2 -fexceptions -sDISABLE_EXCEPTION_CATCHING=0 -sUSE_ZLIB=1
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createDecryptModule -sENVIRONMENT=worker,node
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB
        -sFORCE_FILESYSTEM=1 -lworkerfs.js -lnodefs.js --no-entry
        "-sEXPORTED_FUNCTIONS=['_vd_open','_vd_count','_vd_entry','_vd_run','_vd_decrypt_self','_vd_error','_vd_close']"
        "-sEXPORTED_RUNTIME_METHODS=['FS','WORKERFS','NODEFS','MEMFS','ccall','UTF8ToString']")
    set_target_properties(vita3k_decrypt PROPERTIES SUFFIX ".mjs")
endif()
