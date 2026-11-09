/* Stand-in for PPSSPP's ppsspp_config.h (third_party/at3_standalone_src/atrac3plusdsp.cpp):
 * the EE is none of the SIMD architectures it tests, so the plain C paths are used. */
#pragma once
#define PPSSPP_ARCH(x) 0
