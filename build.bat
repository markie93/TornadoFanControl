@echo off
cd /d "%~dp0"

echo [1/2] Building GUI (PyInstaller)...
pyinstaller ui.spec --noconfirm
if %errorlevel% neq 0 (
    echo.
    echo PyInstaller build FAILED.
    pause
    exit /b 1
)
echo.

echo [2/2] Building installer (Inno Setup)...
set ISCC=
for %%P in (
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    "C:\Program Files\Inno Setup 6\ISCC.exe"
) do if exist %%P set ISCC=%%~P

if "%ISCC%"=="" (
    echo ERROR: Inno Setup 6 not found. Install it from https://jrsoftware.org/isinfo.php
    pause
    exit /b 1
)

"%ISCC%" "dist\Inno_6.iss"
if %errorlevel% neq 0 (
    echo.
    echo Inno Setup build FAILED.
    pause
    exit /b 1
)

echo.
echo Build complete: dist\TornadoFanControllerInstaller.exe
pause
