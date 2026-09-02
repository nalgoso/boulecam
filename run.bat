@echo off
if not exist "%~dp0build\Release\boulecam-desktop.exe" (
    echo [ERROR] No se encuentra 'build\Release\boulecam-desktop.exe'.
    echo Primero debes ejecutar 'build.bat' para compilar el proyecto.
    echo.
    pause
    exit /b 1
)

echo Iniciando BouleCam Desktop Service...
"%~dp0build\Release\boulecam-desktop.exe"
pause
