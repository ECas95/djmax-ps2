/* Stand-in for PPSSPP's Common/MemoryUtil.h, so third_party/at3_standalone_src/mem.cpp
 * builds unchanged. The PS2 port provides these in ps2_at3.cpp (guest-RAM arena). */
#pragma once
#include <stddef.h>

void *AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void *ptr);
