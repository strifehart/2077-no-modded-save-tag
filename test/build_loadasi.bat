@echo off
call "%~dp0..\vsenv.bat" || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT loadasi.cpp /Fe:loadasi.exe
