#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

/* What upstream's configure generates from speexdsp_config_types.h.in on a
   platform with <stdint.h>. Written by hand for WiiCompiled's vendored copy,
   which builds without configure (see runtime/cmake/SpeexDsp.cmake). */
#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
