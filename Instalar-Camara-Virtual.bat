@echo off
title Instalar BouleCam Virtual Camera
echo ========================================================
echo        Instalando BouleCam Virtual Camera
echo ========================================================
echo.

set "DLL_PATH=%~dp0build\Release\boulecam-vcam.dll"
if not exist "%DLL_PATH%" (
    echo [ERROR] No se encuentra '%DLL_PATH%'.
    pause
    exit /b 1
)

echo [1/3] Registrando servidor COM (InprocServer32)...
reg add "HKCU\Software\Classes\CLSID\{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}" /ve /d "BouleCam Virtual Camera" /f >nul
reg add "HKCU\Software\Classes\CLSID\{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}\InprocServer32" /ve /d "%DLL_PATH%" /f >nul
reg add "HKCU\Software\Classes\CLSID\{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}\InprocServer32" /v "ThreadingModel" /d "Both" /f >nul

echo [2/3] Registrando en categoria DirectShow Video Capture (Chrome, OBS, Discord)...
reg add "HKCU\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\BouleCam Virtual Camera" /v "FriendlyName" /d "BouleCam Virtual Camera" /f >nul
reg add "HKCU\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\BouleCam Virtual Camera" /v "CLSID" /d "{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}" /f >nul
reg add "HKCU\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\BouleCam Virtual Camera" /v "FilterData" /t REG_BINARY /d 02000000000020000100000000000000307069330800000000000000010000000000000000000000307479330000000038000000480000007669647300001000800000aa00389b7100000000000000000000000000000000 /f >nul

echo [3/3] Registrando en Media Foundation...
"%~dp0build\Release\register_vcam.exe" --install >nul 2>&1

echo.
echo ========================================================
echo   EXITO: "BouleCam Virtual Camera" REGISTRADA EN WINDOWS
echo ========================================================
echo Disponible en OBS Studio, Chrome, Edge, Discord, Zoom y Teams.
echo.
pause
