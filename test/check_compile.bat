@echo off
rem Compile-checks one rewrite file without linking or touching hook\build:  test\check_compile.bat hook\phys_x.cpp [outdir]
rem (for working on several files at once: each check writes only to its own out dir)
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set OUTD=%~2
if "%OUTD%"=="" set OUTD=%TEMP%\vp_check
if not exist "%OUTD%" mkdir "%OUTD%"
cl /nologo /c /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /I"%~dp0..\..\sdl2\SDL2-2.32.10\include" "%~1" /Fo"%OUTD%\\" || exit /b 1
