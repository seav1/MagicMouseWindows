@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem One-click build for MagicMouse.exe (no .NET, no MSBuild, no .sln).
rem Auto-detects Visual Studio Build Tools via vswhere if cl.exe isn't on PATH.
rem ---------------------------------------------------------------------------

cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
  set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%_VSWHERE%" set "_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%_VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
      set "_VSDIR=%%i"
    )
  )
  if defined _VSDIR if exist "%_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

where cl >nul 2>nul
if errorlevel 1 (
  echo [build.cmd] MSVC compiler ^(cl.exe^) not found. Install "Visual Studio
  echo            Build Tools" with the "Desktop development with C++" workload.
  exit /b 1
)

rem --- sanity: make sure the embedded driver files are present ----------------
for %%F in (drivers\MagicMouse.inf drivers\MagicMouse.sys drivers\MagicMouse.cat) do (
  if not exist "%%F" (
    echo [build.cmd] ERROR: required file is missing: %%F
    echo            The driver files must be committed/uploaded alongside the
    echo            source. Make sure "src\MagicMouse\drivers\" contains
    echo            MagicMouse.inf, MagicMouse.sys and MagicMouse.cat.
    exit /b 1
  )
)

if not exist build mkdir build

echo [build.cmd] Compiling resource (embeds driver files)...
rc /nologo /fo build\MagicMouse.res MagicMouse.rc || exit /b 1

echo [build.cmd] Compiling source...
cl /nologo /O2 /MT /EHsc /W3 /GS- ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN ^
   /Fobuild\ /Fdbuild\ ^
   MagicMouse.cpp build\MagicMouse.res ^
   /link /SUBSYSTEM:WINDOWS /OUT:build\MagicMouse.exe ^
   setupapi.lib user32.lib shell32.lib advapi32.lib gdi32.lib ole32.lib kernel32.lib || exit /b 1

echo.
echo [build.cmd] Built: %~dp0build\MagicMouse.exe
for %%F in ("%~dp0build\MagicMouse.exe") do echo            Size : %%~zF bytes
exit /b 0
