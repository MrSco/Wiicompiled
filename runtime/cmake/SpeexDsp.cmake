# SpeexDSP echo cancellation, noise suppression and resampling for voice chat's
# microphone (src/voice/voice_echo.cpp). The float build of the upstream 1.2.1
# release (https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz,
# SHA-256 8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d) is
# vendored under third_party/speexdsp so disconnected builds compile the same
# sources.
#
# Only the acoustic echo canceller (mdf.c), the preprocessor (preprocess.c,
# filterbank.c), the resampler (resample.c) and the FFT they share (fftwrap.c
# with smallft.c, upstream's default) are vendored, with the headers they
# include. The fixed-point and SIMD variants, the jitter buffer and the test
# programs are left out. speexdsp_config_types.h, which upstream's configure
# generates, is written by hand.
#
# Included from both CMakeLists.txt and cmake/PlatformTests.cmake, because the
# platform-tests configuration returns before the main third-party block.
if(TARGET mkw_speexdsp)
    return()
endif()

set(_mkw_speexdsp_dir "${CMAKE_CURRENT_LIST_DIR}/../third_party/speexdsp")
add_library(mkw_speexdsp STATIC
    "${_mkw_speexdsp_dir}/libspeexdsp/mdf.c"
    "${_mkw_speexdsp_dir}/libspeexdsp/preprocess.c"
    "${_mkw_speexdsp_dir}/libspeexdsp/filterbank.c"
    "${_mkw_speexdsp_dir}/libspeexdsp/fftwrap.c"
    "${_mkw_speexdsp_dir}/libspeexdsp/smallft.c"
    "${_mkw_speexdsp_dir}/libspeexdsp/resample.c")
add_library(mkw::speexdsp ALIAS mkw_speexdsp)
target_include_directories(mkw_speexdsp SYSTEM PUBLIC "${_mkw_speexdsp_dir}/include")
target_include_directories(mkw_speexdsp PRIVATE "${_mkw_speexdsp_dir}/libspeexdsp")
# Upstream's config.h equivalents for a float build. EXPORT is empty because
# the library is linked statically.
target_compile_definitions(mkw_speexdsp PRIVATE
    FLOATING_POINT
    USE_SMALLFT
    EXPORT=)
set_target_properties(mkw_speexdsp PROPERTIES
    C_STANDARD 99
    UNITY_BUILD OFF
    POSITION_INDEPENDENT_CODE ON)
target_compile_options(mkw_speexdsp PRIVATE -w)
