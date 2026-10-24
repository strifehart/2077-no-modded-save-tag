@echo off
call "%~dp0..\vsenv.bat" || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /W4 /std:c++17 hardening.cpp /Fe:hardening.exe user32.lib
