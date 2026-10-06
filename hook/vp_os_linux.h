// vp_os_linux.h -- the native Linux build's OS layer (relink stage R2b, agent B): what vp_os.h's "O" kind is on Linux,
// the Win32 names the port still calls itself (standalone.cpp, viperport.cpp), and what the ELF loader
// (loader/viperport_linux.cpp) tells it. hook/vp_os_linux.cpp has the code; Linux only.
//
// Memory: race.exe's image and every vpos_VirtualAlloc block are kept in a region table (their Windows protections page
// by page, MEM_IMAGE / MEM_PRIVATE, reserved or committed), so VirtualProtect returns the old protection Windows would
// and VirtualQuery answers as Windows does; any other address is looked up in /proc/self/maps.
// Modules: GetModuleHandleA(NULL) is race.exe's image (0x400000) -- the port asks only for that; the port's own module
// is this executable (its handle: the ELF's first byte, __executable_start), whose GetModuleFileNameA is a Windows path
// under the install folder (vp_linux_set_self): viperport.ini and viperport.log beside race.exe, as on Windows, where
// viperport.exe and dinput.dll go beside it.
// Files: the port opens its own files (viperport.log, the session and race recordings, captures) with the C runtime's
// fopen and Windows paths; tools/build_linux.sh links with -Wl,--wrap=fopen (and fopen64), and the wrapper sends a
// Windows path (a drive letter or a backslash) through the path layer (w32_path.h) -- any other path is the host's.
#pragma once
#if defined(_WIN32)
#error "vp_os_linux.h is for the Linux build only"
#endif
#include <stdint.h>

// the port's own module handle (the executable's first byte)
void* vp_linux_self_module();
// the port's own module path, Windows form (e.g. "C:\\viperport"): GetModuleFileNameA of 0 or vp_linux_self_module()
void vp_linux_set_self(const char* windows_path);
// a range the loader mapped (race.exe's image: MEM_IMAGE), every page committed with protection `prot` (PAGE_*); later
// vpos_VirtualProtect calls set each section's own
void vp_linux_note_region(uint32_t base, uint32_t size, uint32_t type, uint32_t prot);
// PAGE_* -> PROT_* (PAGE_EXECUTE* keep read: x86 can't execute without it); -1 for an invalid protection
int vp_linux_unix_prot(uint32_t page_prot);
