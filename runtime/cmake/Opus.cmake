# Opus voice codec for in-game voice chat (src/voice). The portable float
# encoder/decoder of the upstream 1.5.2 release
# (https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz, SHA-256
# 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1) is vendored
# under third_party/opus so disconnected builds compile the same sources.
#
# Only celt/, silk/, silk/float/, src/ and include/ are vendored. The x86/ARM/MIPS
# SIMD variants are left out, like Crypto++'s, to stay independent of the
# runtime's x86-64-v3 policy; the dnn/ tree (deep PLC, DRED, OSCE) is left out
# because none of those optional features are enabled. The upstream demo
# programs are not vendored.
#
# Included from both CMakeLists.txt and cmake/PlatformTests.cmake, because the
# platform-tests configuration returns before the main third-party block.
if(TARGET mkw_opus)
    return()
endif()

set(_mkw_opus_dir "${CMAKE_CURRENT_LIST_DIR}/../third_party/opus")
file(GLOB _mkw_opus_sources CONFIGURE_DEPENDS
    "${_mkw_opus_dir}/celt/*.c"
    "${_mkw_opus_dir}/silk/*.c"
    "${_mkw_opus_dir}/silk/float/*.c"
    "${_mkw_opus_dir}/src/*.c")
add_library(mkw_opus STATIC ${_mkw_opus_sources})
add_library(mkw::opus ALIAS mkw_opus)
target_include_directories(mkw_opus SYSTEM PUBLIC "${_mkw_opus_dir}/include")
target_include_directories(mkw_opus PRIVATE
    "${_mkw_opus_dir}"
    "${_mkw_opus_dir}/celt"
    "${_mkw_opus_dir}/silk"
    "${_mkw_opus_dir}/silk/float")
# Upstream's config.h equivalents for a float build without run-time CPU
# detection. VAR_ARRAYS uses C99 variable-length arrays for scratch buffers.
target_compile_definitions(mkw_opus PRIVATE
    OPUS_BUILD
    VAR_ARRAYS
    HAVE_LRINT
    HAVE_LRINTF
    PACKAGE_VERSION="1.5.2")
set_target_properties(mkw_opus PROPERTIES
    C_STANDARD 99
    UNITY_BUILD OFF
    POSITION_INDEPENDENT_CODE ON)
target_compile_options(mkw_opus PRIVATE -w)
