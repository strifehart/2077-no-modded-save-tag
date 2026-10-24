@echo off
call "%~dp0vsenv.bat" || exit /b 1
if not exist "%~dp0build" mkdir "%~dp0build"
cd /d "%~dp0build"
cl /nologo /O2 /MT /EHsc /W4 /std:c++17 /LD ..\src\NoModdedSaveTag.cpp /Fe:NoModdedSaveTag.asi /link /DLL user32.lib
