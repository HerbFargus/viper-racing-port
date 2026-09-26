@echo off
rem Apply the recovered class layouts (out\types.tsv, from tools\recover_types.py) to the Ghidra project.
rem   tools\ghidra_types.bat
for %%I in ("%~dp0..") do set ROOT=%%~fI
for %%I in ("%~dp0..\..\ghidra\ghidra_12.1.4_PUBLIC") do set GHIDRA=%%~fI
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\out\ghidra" ViperRacing -process race_v10.exe -noanalysis -scriptPath "%ROOT%\tools" -postScript ApplyTypes.java "%ROOT%\out\types.tsv" "%ROOT%\out\types_skipped.txt"
