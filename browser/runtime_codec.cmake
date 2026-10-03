# Audio/video codecs for the HLE codec modules (SceNgs, SceAudiodec,
# SceVideodec): vita3k/codec and vita3k/ngs over FFmpeg and LibAtrac9.
# Include from runtime_hle.cmake; defines vita3k_web_codec.
#
# Desktop links Vita3K's prebuilt FFmpeg (external/ffmpeg). The browser builds
# the same FFmpeg release from source with Emscripten for this build's memory
# model, configured with desktop's decoder set (external/ffmpeg/ffmpeg.patch)
# and no threads, asm or autodetected system libraries. The headers stay
# external/ffmpeg/include, as on desktop (aac.cpp uses an internal header).
# Like the shader toolchain's npm install, this fetches the source tarball
# once; VITA3K_WEB_FFMPEG_URL may point at a local mirror.
include_guard(GLOBAL)
include(ExternalProject)

set(_CODEC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vita3k")
set(_CODEC_EXT "${CMAKE_CURRENT_LIST_DIR}/../external")

set(VITA3K_WEB_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz" CACHE STRING
    "FFmpeg 7.1 source tarball (the release of external/ffmpeg/include)")
set(_ffmpeg_prefix "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
get_filename_component(_em_bin "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(VITA3K_EMCONFIGURE emconfigure HINTS "${_em_bin}" REQUIRED)
find_program(VITA3K_EMMAKE emmake HINTS "${_em_bin}" REQUIRED)
set(_ffmpeg_cflags "-O2")
set(_ffmpeg_ldflags "")
if(VITA3K_WEB_MEMORY64)
    set(_ffmpeg_cflags "-sMEMORY64=1 -O2")
    set(_ffmpeg_ldflags "-sMEMORY64=1")
endif()
set(_ffmpeg_libs avformat avcodec swscale swresample avutil)
set(_ffmpeg_byproducts)
foreach(_lib IN LISTS _ffmpeg_libs)
    list(APPEND _ffmpeg_byproducts "${_ffmpeg_prefix}/lib/lib${_lib}.a")
endforeach()
ExternalProject_Add(vita3k_web_ffmpeg_build
    URL "${VITA3K_WEB_FFMPEG_URL}"
    URL_HASH SHA256=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${_ffmpeg_prefix}"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND "${VITA3K_EMCONFIGURE}" ./configure "--prefix=${_ffmpeg_prefix}"
        --enable-cross-compile --target-os=none --arch=x86_64
        "--cc=${CMAKE_C_COMPILER}" "--cxx=${CMAKE_CXX_COMPILER}" "--ar=${CMAKE_AR}"
        "--ranlib=${CMAKE_RANLIB}" "--nm=${CMAKE_NM}"
        "--extra-cflags=${_ffmpeg_cflags}" "--extra-ldflags=${_ffmpeg_ldflags}"
        --disable-asm --disable-inline-asm --disable-x86asm --disable-programs --disable-doc
        --disable-debug --disable-pthreads --disable-w32threads --disable-os2threads
        --disable-network --disable-autodetect --disable-everything
        --disable-avdevice --disable-avfilter --disable-postproc
        --enable-decoder=aac,aac_latm,atrac3,atrac3p,atrac9,mp3,pcm_s16le,pcm_s8
        --enable-decoder=h264,mpeg4,mpeg2video,mjpeg,mjpegb
        --enable-encoder=pcm_s16le,mjpeg
        --enable-demuxer=h264,m4v,mp3,mpegvideo,mpegps,mjpeg,mov,aac,oma,pcm_s16le,pcm_s8,wav
        --enable-parser=h264,mpeg4video,mpegaudio,mpegvideo,mjpeg,aac,aac_latm
        --enable-protocol=file --enable-bsf=mjpeg2jpeg --enable-swresample --enable-swscale
    BUILD_COMMAND "${VITA3K_EMMAKE}" make -j8
    INSTALL_COMMAND "${VITA3K_EMMAKE}" make install
    BUILD_BYPRODUCTS ${_ffmpeg_byproducts}
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
)
add_library(vita3k_web_ffmpeg INTERFACE)
add_dependencies(vita3k_web_ffmpeg vita3k_web_ffmpeg_build)
target_include_directories(vita3k_web_ffmpeg INTERFACE "${_CODEC_EXT}/ffmpeg/include")
target_link_libraries(vita3k_web_ffmpeg INTERFACE ${_ffmpeg_byproducts})

file(GLOB _atrac9_sources "${_CODEC_EXT}/LibAtrac9/C/src/*.c")
add_library(vita3k_web_libatrac9 STATIC ${_atrac9_sources})
target_include_directories(vita3k_web_libatrac9 PUBLIC "${_CODEC_EXT}/LibAtrac9/C/src")

set(VITA3K_WEB_CODEC_SOURCES
    "${_CODEC_ROOT}/codec/src/atrac9.cpp"
    "${_CODEC_ROOT}/codec/src/decoder.cpp"
    "${_CODEC_ROOT}/codec/src/aac.cpp"
    "${_CODEC_ROOT}/codec/src/h264.cpp"
    "${_CODEC_ROOT}/codec/src/mjpeg.cpp"
    "${_CODEC_ROOT}/codec/src/mp3.cpp"
    "${_CODEC_ROOT}/codec/src/pcm.cpp"
    "${_CODEC_ROOT}/codec/src/player.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/atrac9.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/compressor.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/delay.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/distortion.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/envelope.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/equalizer.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/filter.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/generator.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/mixer.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/output.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/pauser.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/pitchshift.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/player.cpp"
    "${_CODEC_ROOT}/ngs/src/modules/reverb.cpp"
    "${_CODEC_ROOT}/ngs/src/definitions.cpp"
    "${_CODEC_ROOT}/ngs/src/ngs.cpp"
    "${_CODEC_ROOT}/ngs/src/rate_resampler.cpp"
    "${_CODEC_ROOT}/ngs/src/route.cpp"
    "${_CODEC_ROOT}/ngs/src/scheduler.cpp"
)
