// linux_todo.cpp -- the native Linux build's placeholder abort (relink stage R2b; linux_todo.h). The stubs that stood
// here while the Linux pieces were written (the ELF loader and OS layer, SEH and the threads' fs: TIB, platform, splash
// and GL) are all real now; what's left is vp_r2b_todo, which vp_os.cpp calls on Linux paths that can't be reached, so
// nothing there silently does the wrong thing.
#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>                                           // hook/linux_inc -> win32_compat.h
#include "vp_os.h"
#include "w32_kernel.h"

void vp_r2b_todo(const char* what) {
    fprintf(stderr, "R2b TODO: %s -- not written yet on Linux\n", what);
    fflush(stderr);
    abort();
}
#endif  // !_WIN32
