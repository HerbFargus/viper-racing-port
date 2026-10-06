@echo off
rem Builds loader\build-gcc\viperport.exe with GCC (mingw-w64 i686, MSYS2's C:\msys64\mingw32): the loader of
rem loader\build_loader.bat (32-bit, GUI, fixed at 0x00800000 so race.exe's 0x400000 is free, no ASLR, stack 0x100000),
rem built by the second compiler (relink stage R1). The port DLL for it is hook\build_gcc.bat's:
rem   loader\build-gcc\viperport.exe --check <race.exe> --dll hook\build-gcc\dinput.dll
rem The C and C++ runtimes are linked in (-static), so the exe needs nothing of MSYS2's at run time.
setlocal
set GCC=C:\msys64\mingw32\bin
if not exist "%GCC%\g++.exe" echo build_loader_gcc: no GCC at %GCC% (MSYS2: pacman -S mingw-w64-i686-gcc) & exit /b 1
set PATH=%GCC%;%PATH%
set OUT=%~dp0build-gcc
if not exist "%OUT%" mkdir "%OUT%"
g++ -m32 -O2 -march=i686 -mfpmath=387 -fexcess-precision=standard -fno-strict-aliasing -masm=intel -std=c++17 -fms-extensions -mwindows -static -o "%OUT%\viperport.exe" "%~dp0viperport_main.cpp" -Wl,--image-base,0x00800000 -Wl,--disable-dynamicbase -Wl,--disable-reloc-section -Wl,--tsaware -Xlinker --stack -Xlinker 0x100000,0x1000 -Wl,--major-os-version,4 -Wl,--minor-os-version,0 -Wl,-Map,"%OUT%\viperport.map" -lkernel32 -luser32 -lgdi32 -lbcrypt -ladvapi32 || exit /b 1
rem race.exe says it was made for Windows 4.0 (OS and subsystem version 4.0), and Windows treats it so: thin old-style
rem window frames, among other things. viperport.exe says the same, so the game gets what race.exe gets: the subsystem
rem version is set here, after the link, as build_loader.bat does it.
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $f='%OUT%\viperport.exe'; $b=[IO.File]::ReadAllBytes($f); $o=[BitConverter]::ToInt32($b,0x3c)+24+48; $b[$o]=4; $b[$o+1]=0; $b[$o+2]=0; $b[$o+3]=0; [IO.File]::WriteAllBytes($f,$b)" || exit /b 1
