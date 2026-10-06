// linux_todo.h -- the native Linux build's placeholders (relink stage R2b, agent A): what agents B (the ELF loader and
// the OS layer), C (SEH, signals, the threads' fs: TIB) and D (platform, splash, GL, two_copies) will write.
// vp_r2b_todo(what) prints "R2b TODO: <what>" to stderr and aborts; hook/linux_todo.cpp has it and every stub, each
// marked with its owner. Linux only: nothing on Windows includes this.
#pragma once
#if defined(_WIN32)
#error "linux_todo.h is for the Linux build only"
#endif
[[noreturn]] void vp_r2b_todo(const char* what);
