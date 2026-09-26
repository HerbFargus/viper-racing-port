@echo off
rem Decompile named functions from the analysed project to out\decomp_sample.c
rem   tools\ghidra_decompile.bat Obstacle::Update IdealLine::advance_bead ...
for %%I in ("%~dp0..") do set ROOT=%%~fI
for %%I in ("%~dp0..\..\ghidra\ghidra_12.1.4_PUBLIC") do set GHIDRA=%%~fI
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\out\ghidra" ViperRacing -process race_v10.exe -noanalysis -readOnly -scriptPath "%ROOT%\tools" -postScript DecompileSample.java "%ROOT%\out\decomp_sample.c" %*
