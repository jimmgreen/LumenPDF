@echo off
setlocal
set "VSLANG=1033"
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio C++ tools not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PS_EXE=powershell"
where pwsh >nul 2>nul && set "PS_EXE=pwsh"
set "PSModulePath=%ProgramFiles%\WindowsPowerShell\Modules;%SystemRoot%\system32\WindowsPowerShell\v1.0\Modules"
%PS_EXE% -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap.ps1" || exit /b 1
cmake -S "%ROOT%." -B "%ROOT%build\app" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%ROOT%build\app" || exit /b 1
ctest --test-dir "%ROOT%build\app" --output-on-failure || exit /b 1


