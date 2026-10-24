@echo off
rem Sets up the 64-bit MSVC command-line environment from any Visual Studio 2022 or later
rem install (Community, Professional, Enterprise or Build Tools) that has the C++ tools.
rem Called by build.bat and the test\build_*.bat scripts; returns a non-zero exit code on failure.

set "NMST_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%NMST_VSWHERE%" (
	echo ERROR: Visual Studio Installer not found. Install Visual Studio 2022 or later with the
	echo        "Desktop development with C++" workload.
	exit /b 1
)

set "NMST_VSDIR="
for /f "usebackq delims=" %%i in (`"%NMST_VSWHERE%" -latest -products * -version [17.0^,^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NMST_VSDIR=%%i"
if not defined NMST_VSDIR (
	echo ERROR: no Visual Studio 2022 or later with the C++ tools was found. Add the
	echo        "Desktop development with C++" workload in the Visual Studio Installer.
	exit /b 1
)

call "%NMST_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || (
	echo ERROR: the MSVC compiler could not be set up from "%NMST_VSDIR%".
	exit /b 1
)
exit /b 0
