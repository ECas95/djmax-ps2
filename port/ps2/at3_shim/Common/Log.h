/* Stand-in for PPSSPP's Common/Log.h (third_party/at3_standalone_src/compat.cpp): decoder
 * errors and warnings go to the EE TTY, debug chatter is dropped. */
#pragma once

extern "C" void psp_printf(const char *fmt, ...);

#define ERROR_LOG(cat, ...) psp_printf("[at3] " __VA_ARGS__), psp_printf("\n")
#define WARN_LOG(cat, ...)  psp_printf("[at3] " __VA_ARGS__), psp_printf("\n")
#define INFO_LOG(cat, ...)  ((void)0)
#define DEBUG_LOG(cat, ...) ((void)0)
