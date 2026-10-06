@echo off
rem Builds hook\build-gcc\dinput.dll (32-bit) with GCC (mingw-w64 i686, MSYS2's C:\msys64\mingw32), against SDL2 in
rem ..\..\sdl2: the same sources as hook\build.bat (relink stage R1: the second build beside MSVC's).
rem   -fexcess-precision=standard rounds a float or double at every assignment, cast and return, as MSVC /fp:precise does
rem   on the x87 (see compiler.h); -mfpmath=387 -march=i686 keep everything on the x87, as /arch:IA32 does.
rem   -fno-strict-aliasing: the rewrites read a float's bits through an int (and back) on purpose, as the original does;
rem   MSVC never optimises on type-based aliasing, and with this GCC doesn't either.
rem   SDL2.dll is linked normally (GNU ld has no delay-load): the DLL needs SDL2.dll beside it to load at all.
rem   libgcc, libstdc++ and winpthread are linked in (-static), so the DLL needs nothing of MSYS2's at run time.
rem   DirectInputCreateA is __stdcall (_DirectInputCreateA@16): viperport.def's undecorated name binds to it through ld's
rem   stdcall fixup, so the export is DirectInputCreateA, as MSVC's.
rem The sources compile in parallel (one g++ a processor: the PowerShell at the end of this file) into build-gcc\obj,
rem then link in build.bat's order, with a link map (build-gcc\dinput.map).
setlocal
set GCC=C:\msys64\mingw32\bin
if not exist "%GCC%\g++.exe" echo build_gcc: no GCC at %GCC% (MSYS2: pacman -S mingw-w64-i686-gcc) & exit /b 1
set PATH=%GCC%;%PATH%
set SDL=%~dp0..\..\sdl2\SDL2-2.32.10
set VP_OUT=%~dp0build-gcc
set VP_CFLAGS=-m32 -O2 -march=i686 -mfpmath=387 -fexcess-precision=standard -fno-strict-aliasing -masm=intel -std=c++17 -fms-extensions -Wno-invalid-offsetof -I"%SDL%\include"
set VP_SOURCES=viperport.cpp port.cpp replay.cpp session.cpp standalone.cpp net_*.cpp crt_*.cpp edit_*.cpp phys_*.cpp wld_*.cpp krn_*.cpp gx_*.cpp snd_*.cpp ui_*.cpp menu_*.cpp root_*.cpp career_*.cpp paint_*.cpp w32_*.cpp platform.cpp gl_table.cpp gl_core.cpp gl_dxgi.cpp ddraw_gl.cpp dsound_sdl.cpp
if not exist "%VP_OUT%\obj" mkdir "%VP_OUT%\obj"
pushd "%~dp0" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText('%~f0'); iex $s.Substring($s.LastIndexOf('#' + 'COMPILE'))"
if errorlevel 1 popd & exit /b 1
g++ -m32 -shared -static -o "%VP_OUT%\dinput.dll" @"%VP_OUT%\objects.rsp" viperport.def -Wl,--enable-stdcall-fixup -Wl,--image-base,0x10000000 -Wl,-Map,"%VP_OUT%\dinput.map" -lkernel32 -luser32 -lopengl32 -ldxguid -lwinmm "%SDL%\lib\x86\SDL2.lib"
if errorlevel 1 popd & exit /b 1
popd
copy /y "%SDL%\lib\x86\SDL2.dll" "%VP_OUT%\SDL2.dll" >nul
exit /b 0

#COMPILE: every source to build-gcc\obj\<name>.o, as many at a time as there are processors; objects.rsp lists them
$ErrorActionPreference = 'Stop'
$obj = Join-Path $env:VP_OUT 'obj'
$srcs = @(foreach ($p in $env:VP_SOURCES.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
    Get-ChildItem -Path $p | Sort-Object Name | ForEach-Object { $_.Name } })
$max = [Environment]::ProcessorCount
$jobs = @()
foreach ($s in $srcs) {
    while (@($jobs | Where-Object { -not $_.P.HasExited }).Count -ge $max) { Start-Sleep -Milliseconds 50 }
    $o = Join-Path $obj ([IO.Path]::ChangeExtension($s, '.o'))
    $p = Start-Process -FilePath cmd.exe -ArgumentList "/c g++ $env:VP_CFLAGS -c $s -o `"$o`" 2>`"$o.log`"" -NoNewWindow -PassThru
    $null = $p.Handle
    $jobs += [pscustomobject]@{ P = $p; S = $s; O = $o }
}
$failed = 0
foreach ($j in $jobs) {
    $j.P.WaitForExit()
    $log = Get-Content -Raw "$($j.O).log" -ErrorAction SilentlyContinue
    if ($log) { Write-Host $log }
    if ($j.P.ExitCode -ne 0) { Write-Host "build_gcc: $($j.S) failed"; $failed++ }
}
if ($failed) { exit 1 }
$srcs | ForEach-Object { 'build-gcc/obj/' + [IO.Path]::ChangeExtension($_, '.o') } | Set-Content -Encoding Ascii (Join-Path $env:VP_OUT 'objects.rsp')
exit 0
