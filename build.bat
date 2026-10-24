@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0build"
cl /nologo /O2 /MT /EHsc /W4 /std:c++17 /LD ..\src\NoModdedSaveTag.cpp /Fe:NoModdedSaveTag.asi /link /DLL user32.lib
