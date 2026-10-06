// w32_seh.h -- the native Linux build's structured exception handling (w32_seh.cpp, relink stage R2b): what the loader
// and the thread stand-ins call so that every thread that runs the game's code has a Windows thread block behind fs and
// its faults become Windows exceptions. Nothing here on Windows (the OS does all of it there).
#pragma once
#ifndef _WIN32
#include <stddef.h>

// The calling thread's TIB behind fs (fs:[0] the SEH chain, -1; fs:[4]/[8] its stack's bounds; fs:[0x18] the block's
// own address; fs:[0x20]/[0x24] process and thread id; fs:[0x30] a PEB whose ImageBaseAddress is race.exe's 0x400000),
// a signal stack of its own, and (once per process) the fault signal handlers. Idempotent. The ELF loader calls it on
// the main thread before race.exe's entry; w32_CreateThread's threads call it before their start routine; any other
// thread that runs the port's or the game's code (SDL's audio thread, if it does) should call it first. A thread's
// block is freed when the thread ends (the main thread's never is).
void w32_seh_thread_begin();

// The stack bounds the dispatcher checks SEH records against (fs:[8] limit, fs:[4] base) when the thread runs the game
// on a stack other than the one pthread gave it.
void w32_seh_set_stack_bounds(void* limit, void* base);

// IsBadReadPtr's question, without a signal handler of the caller's own (that would replace the SEH one): true when
// all n bytes at p can be read. A fault here is not an exception: the vectored handlers don't see it.
bool w32_seh_readable(const void* p, size_t n);
#endif
