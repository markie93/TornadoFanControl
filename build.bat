@echo off
setlocal
cd /d "%~dp0"

set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

echo [1/3] Building C++ (Release x64)...
%MSBUILD% "CPP\FanControl\FanControl.sln" /p:Configuration=Release /p:Platform=x64 /t:Build /nologo /verbosity:minimal
if %errorlevel% neq 0 (
    echo C++ build FAILED.
    pause & exit /b 1
)

echo [2/3] Building GUI (PyInstaller)...
python -m PyInstaller ui.spec --noconfirm
if %errorlevel% neq 0 (
    echo PyInstaller build FAILED.
    pause & exit /b 1
)

echo [3/3] Building installer (Inno Setup)...
set ISCC=
for %%P in (
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    "C:\Program Files\Inno Setup 6\ISCC.exe"
) do if exist %%P set ISCC=%%~P

if "%ISCC%"=="" (
    echo ERROR: Inno Setup 6 not found. Install from https://jrsoftware.org/isinfo.php
    pause & exit /b 1
)

"%ISCC%" "dist\Inno_6.iss"
if %errorlevel% neq 0 (
    echo Inno Setup build FAILED.
    pause & exit /b 1
)

echo.
echo Done. Installer: dist\TornadoFanControllerInstaller.exe
endlocal
pause
