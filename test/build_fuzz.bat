@echo off
rem Builds fuzz.exe: pure rewrites against the originals in out\race_v10.exe (see test\fuzz.cpp).
rem   test\build_fuzz.bat                      all of hook\phys_*.cpp, into test\build
rem   test\build_fuzz.bat OUTDIR file.cpp...   just these files, into OUTDIR (for several at once)
rem Linked at 0x10000000 without ASLR, so race.exe's own base, 0x400000, is free for its code.
rem Built with VP_FAITHFUL: the rewrites exactly as the originals, fixes off (docs/PORTING.md, "Fixes").
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set OUTD=%~1
if "%OUTD%"=="" set OUTD=%~dp0build
set FILES="%~dp0..\hook\phys_*.cpp"
if not "%~2"=="" set FILES=%2 %3 %4 %5 %6 %7 %8 %9
if not exist "%OUTD%" mkdir "%OUTD%"
cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /DVP_FUZZ /DVP_FAITHFUL "%~dp0fuzz.cpp" %FILES% /Fo"%OUTD%\\" /Fe"%OUTD%\fuzz.exe" /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86 || exit /b 1
