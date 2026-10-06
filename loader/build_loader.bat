@echo off
rem Builds loader\build\viperport.exe (32-bit, GUI, fixed at 0x00800000 so race.exe's 0x400000 is free).
rem   loader\build_loader.bat         the loader
rem   loader\build_loader.bat dll     and a copy of the port DLL with the standalone entry (hook\*.cpp, as hook\build.bat
rem                                   builds it plus standalone.cpp and crt_start.cpp) into loader\build\, with SDL2.dll,
rem                                   for `loader\build\viperport.exe --check <race.exe>` -- hook\build is never touched
rem It needs only standalone.h of the DLL. The game runs from viperport.exe placed beside race.exe, with the port's
rem dinput.dll (built by hook\build.bat) and SDL2.dll there.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 "%~dp0viperport_main.cpp" /Fo"%OUT%\\" /Fe"%OUT%\viperport.exe" /link /SUBSYSTEM:WINDOWS /OSVERSION:4.00 /BASE:0x00800000 /FIXED /DYNAMICBASE:NO /STACK:0x100000,0x1000 /MACHINE:X86 kernel32.lib user32.lib gdi32.lib bcrypt.lib advapi32.lib || exit /b 1
rem race.exe says it was made for Windows 4.0 (OS and subsystem version 4.0), and Windows treats it so: thin old-style
rem window frames, among other things. viperport.exe says the same, so the game gets what race.exe gets (the linker goes
rem no lower than 5.01: the subsystem version is set here, after the link).
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $f='%OUT%\viperport.exe'; $b=[IO.File]::ReadAllBytes($f); $o=[BitConverter]::ToInt32($b,0x3c)+24+48; $b[$o]=4; $b[$o+1]=0; $b[$o+2]=0; $b[$o+3]=0; [IO.File]::WriteAllBytes($f,$b)" || exit /b 1
if /i not "%~1"=="dll" exit /b 0
set H=%~dp0..\hook
set SDL=%~dp0..\..\sdl2\SDL2-2.32.10
if not exist "%OUT%\dll" mkdir "%OUT%\dll"
cl /nologo /O2 /arch:IA32 /fp:precise /MT /LD /W3 /EHsc /std:c++17 /I"%SDL%\include" "%H%\viperport.cpp" "%H%\port.cpp" "%H%\replay.cpp" "%H%\session.cpp" "%H%\standalone.cpp" "%H%\net_*.cpp" "%H%\crt_*.cpp" "%H%\edit_*.cpp" "%H%\phys_*.cpp" "%H%\wld_*.cpp" "%H%\krn_*.cpp" "%H%\gx_*.cpp" "%H%\snd_*.cpp" "%H%\ui_*.cpp" "%H%\menu_*.cpp" "%H%\root_*.cpp" "%H%\career_*.cpp" "%H%\paint_*.cpp" "%H%\w32_*.cpp" "%H%\platform.cpp" "%H%\gl_table.cpp" "%H%\gl_core.cpp" "%H%\gl_dxgi.cpp" "%H%\ddraw_gl.cpp" "%H%\dsound_sdl.cpp" /Fo"%OUT%\dll\\" /link /DEF:"%H%\viperport.def" /OUT:"%OUT%\dinput.dll" /MAP:"%OUT%\dinput.map" /MACHINE:X86 kernel32.lib user32.lib opengl32.lib dxguid.lib winmm.lib "%SDL%\lib\x86\SDL2.lib" delayimp.lib /DELAYLOAD:SDL2.dll || exit /b 1
copy /y "%SDL%\lib\x86\SDL2.dll" "%OUT%\SDL2.dll" >nul
