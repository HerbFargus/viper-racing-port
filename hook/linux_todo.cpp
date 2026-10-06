// linux_todo.cpp -- the native Linux build's placeholders (relink stage R2b, agent A; linux_todo.h). Every function
// below is one agent B, C or D of R2b will provide for real; until then it prints its name and aborts, so the port
// links (tools/build_linux.sh's out-linux/viperport-link-test) and nothing silently does the wrong thing. Each owner
// deletes their stubs as they write the real thing (or moves the call site onto vp_os.h); the file ends empty.
// Besides these, vp_r2b_todo("...") calls sit inline where an #error used to mark an R2b gap (platform.cpp,
// viperport.cpp, vp_os.cpp): see the report of agent A.
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

#define TODO(owner, name) vp_r2b_todo(name " (agent " owner ")")

// ---- vp_os.h's OS kind: agent B's are written (hook/vp_os_linux.cpp) ----------------------------------------------------------

// ---- Windows functions the port still calls by name on Linux ---------------------------------------------------------------
// standalone.cpp (the int3 fill, its audit and self-test, the int3 handler, the game's start) and viperport.cpp
// (DllMain's work, the environment) call these directly; on Linux they become vpos_ calls (vp_os.h) or Linux code in
// the loader's integration -- agent B's are in hook/vp_os_linux.cpp; the exception ones are agent C's.
extern "C" {
}
#endif  // !_WIN32
