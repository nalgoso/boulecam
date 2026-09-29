@echo off
setlocal enabledelayedexpansion
title Instalar BouleCam Virtual Camera

echo ========================================================
echo        Instalando BouleCam Virtual Camera
echo ========================================================
echo.

:: Verificar elevacion de Administrador
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [INFO] Se requieren permisos de Administrador para registrar la camara.
    echo Solicitando elevacion UAC...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

set "DLL_PATH=%~dp0build\Release\boulecam-vcam.dll"
if not exist "%DLL_PATH%" (
    echo [ERROR] No se encuentra '%DLL_PATH%'.
    pause
    exit /b 1
)

:: Limpiar entradas invalidas
reg delete "HKCU\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\BouleCam Virtual Camera" /f >nul 2>&1

echo [1/2] Registrando servidor COM de BouleCam en Windows...
regsvr32.exe /s "%DLL_PATH%"

echo [2/2] Registrando en Windows Media Foundation Frame Server...
"%~dp0build\Release\register_vcam.exe" --install

echo.
echo ========================================================
echo   EXITO: "BouleCam Virtual Camera" REGISTRADA EN WINDOWS
echo ========================================================
echo Disponible en OBS Studio ("Dispositivo de captura de video"),
echo Zoom, Google Meet, Microsoft Teams, Discord y navegadores.
echo.
pause
