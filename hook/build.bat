@echo off
rem Builds hook\build\dinput.dll (32-bit) with the Visual Studio Build Tools, against SDL2 in ..\..\sdl2.
rem SDL2.dll is delay-loaded: the DLL loads without it, and only needs it when viperport.ini turns SDL on.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set SDL=%~dp0..\..\sdl2\SDL2-2.32.10
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /O2 /arch:IA32 /fp:precise /MT /LD /W3 /EHsc /std:c++17 /I"%SDL%\include" "%~dp0viperport.cpp" "%~dp0port.cpp" "%~dp0replay.cpp" "%~dp0session.cpp" "%~dp0net_*.cpp" "%~dp0crt_*.cpp" "%~dp0phys_*.cpp" "%~dp0wld_*.cpp" "%~dp0krn_*.cpp" "%~dp0gx_*.cpp" "%~dp0snd_*.cpp" "%~dp0ui_*.cpp" "%~dp0menu_*.cpp" "%~dp0root_*.cpp" "%~dp0career_*.cpp" "%~dp0paint_*.cpp" "%~dp0platform.cpp" "%~dp0gl_table.cpp" "%~dp0gl_core.cpp" "%~dp0gl_dxgi.cpp" "%~dp0ddraw_gl.cpp" "%~dp0dsound_sdl.cpp" /Fo"%~dp0build\\" /link /DEF:"%~dp0viperport.def" /OUT:"%~dp0build\dinput.dll" /MAP:"%~dp0build\dinput.map" /MACHINE:X86 kernel32.lib user32.lib opengl32.lib dxguid.lib winmm.lib "%SDL%\lib\x86\SDL2.lib" delayimp.lib /DELAYLOAD:SDL2.dll || exit /b 1
copy /y "%SDL%\lib\x86\SDL2.dll" "%~dp0build\SDL2.dll" >nul
