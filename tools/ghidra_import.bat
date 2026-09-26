@echo off
rem Import out\race_v10.exe into a Ghidra project at out\ghidra, applying the linker map before analysis.
rem   tools\ghidra_import.bat [path\to\ghidra]
set GHIDRA=%~1
if "%GHIDRA%"=="" for %%I in ("%~dp0..\..\ghidra\ghidra_12.1.4_PUBLIC") do set GHIDRA=%%~fI
for %%I in ("%~dp0..") do set ROOT=%%~fI
if not exist "%ROOT%\out\ghidra" mkdir "%ROOT%\out\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\out\ghidra" ViperRacing -import "%ROOT%\out\race_v10.exe" -overwrite -scriptPath "%ROOT%\tools" -preScript ApplyViperMap.java "%ROOT%\out\symbols.csv" -max-cpu 8
