// linux_todo.h -- the native Linux build's placeholder abort (from relink stage R2b): vp_r2b_todo(what) prints
// "R2b TODO: <what>" to stderr and aborts. vp_os.cpp calls it on the Linux paths that have no stand-in and can't be
// reached (missing(), wait, set_event, find_next, find_close); hook/linux_todo.cpp has it. Linux only: nothing on
// Windows includes this.
#pragma once
#if defined(_WIN32)
#error "linux_todo.h is for the Linux build only"
#endif
[[noreturn]] void vp_r2b_todo(const char* what);
