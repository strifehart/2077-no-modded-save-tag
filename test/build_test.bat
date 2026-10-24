@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /W4 /std:c++17 harness.cpp /Fe:harness.exe user32.lib
