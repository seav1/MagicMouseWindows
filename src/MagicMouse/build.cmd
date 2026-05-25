@echo off
setlocal EnableExtensions

rem ---------------------------------------------------------------------------
rem One-click build for MagicMouse.exe (no .NET, no MSBuild, no Visual Studio
rem solution files - just cl.exe + rc.exe).
rem
rem If you already have a Developer Command Prompt for VS, just run this.
rem Otherwise this script auto-detects Visual Studio Build Tools via vswhere.
rem ---------------------------------------------------------------------------

where cl >nul 2>nul
if errorlevel 1 (
  set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%_VSWHERE%" set "_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%_VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      set "_VSDIR=%%i"
    )
  )
  if defined _VSDIR (
    if exist "%_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
      call "%_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
  )
)

where cl >nul 2>nul
if errorlevel 1 (
  echo [build.cmd] MSVC compiler ^(cl.exe^) not found.
  echo            Install "Visual Studio Build Tools" with the
  echo            "Desktop development with C++" workload, or run this
  echo            script from an existing Developer Command Prompt.
  exit /b 1
)

if not exist build mkdir build
pushd build

echo [build.cmd] Compiling resource...
rc /nologo /fo MagicMouse.res ..\MagicMouse.rc || (popd & exit /b 1)

echo [build.cmd] Compiling source...
cl /nologo /O2 /MT /EHsc /W3 /GS- ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN ^
   ..\MagicMouse.cpp MagicMouse.res ^
   /link /SUBSYSTEM:WINDOWS /OUT:MagicMouse.exe ^
   setupapi.lib user32.lib shell32.lib advapi32.lib gdi32.lib ole32.lib kernel32.lib || (popd & exit /b 1)

popd

echo.
echo [build.cmd] Built: %~dp0build\MagicMouse.exe
for %%F in ("%~dp0build\MagicMouse.exe") do echo            Size : %%~zF bytes
exit /b 0
