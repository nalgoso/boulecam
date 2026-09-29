@echo off
setlocal enabledelayedexpansion
title Configurar Dispositivos Nativos BouleCam (Video y Audio)

echo ========================================================
echo        Registrando Dispositivos Nativos BouleCam
echo           (Camara Virtual + Audio para OBS)
echo ========================================================
echo.

:: 1. Verificar elevacion de Administrador
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [INFO] Se requieren permisos de Administrador para registrar la camara en Windows.
    echo Solicitando elevacion UAC...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

if not exist "%~dp0build\Release\boulecam-vcam.dll" (
    echo [ERROR] No se encuentra '%~dp0build\Release\boulecam-vcam.dll'.
    echo Primero debes compilar el proyecto ejecutando 'build.bat'.
    echo.
    pause
    exit /b 1
)

:: 2. Limpiar registros DirectShow invalidos previos
reg delete "HKCU\Software\Classes\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance\BouleCam Virtual Camera" /f >nul 2>&1

:: 3. Registrar DLL COM en Windows
echo [1/4] Registrando servidor COM de BouleCam Virtual Camera...
regsvr32.exe /s "%~dp0build\Release\boulecam-vcam.dll"

:: 4. Activar Camara Virtual en Windows Media Foundation Frame Server
echo [2/4] Activando Virtual Camera nativa en Windows...
"%~dp0build\Release\register_vcam.exe" --install

:: 5. Configurar Reglas de Firewall de Windows
echo [3/4] Configurando Reglas de Firewall de Windows...
netsh advfirewall firewall delete rule name="BouleCam Streaming" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Streaming" dir=in action=allow protocol=TCP localport=8088,8090 enable=yes >nul 2>&1
netsh advfirewall firewall delete rule name="BouleCam Discovery UDP" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Discovery UDP" dir=in action=allow protocol=UDP localport=8089 enable=yes >nul 2>&1
netsh advfirewall firewall delete rule name="BouleCam Desktop App" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Desktop App" dir=in action=allow program="%~dp0build\Release\boulecam-desktop.exe" enable=yes >nul 2>&1

:: 6. Verificar / Instalar Audio Virtual
echo [4/4] Verificando soporte de Audio Virtual nativo...
powershell -Command "Get-CimInstance Win32_SoundDevice | Where-Object { $_.Name -match 'CABLE|VB-Audio' } | Select-Object -First 1 Name" > "%TEMP%\vbcable_reg_check.txt" 2>nul
set /p CABLE_FOUND=<"%TEMP%\vbcable_reg_check.txt"
del "%TEMP%\vbcable_reg_check.txt" 2>nul

if "!CABLE_FOUND!"=="" (
    echo.
    echo [INFO] No se detecto un Cable de Audio Virtual.
    echo Deseas instalar el Cable de Audio Virtual ahora para que el microfono
    echo del telefono aparezca como dispositivo nativo en OBS Studio? (S/N)
    set /p RESP="Respuesta [S/N]: "
    if /i "!RESP!"=="S" (
        call "%~dp0Instalar-Audio-Virtual.bat"
    )
) else (
    echo [OK] Dispositivo de Audio Virtual activo: !CABLE_FOUND!
)

echo.
echo ========================================================
echo   DISPOSITIVOS NATIVOS CONFIGURADOS CON EXITO PARA OBS!
echo ========================================================
echo.
echo 📹 VIDEO (Camara Web Nativa):
echo    En OBS: [+] Fuentes ^> "Dispositivo de captura de video"
echo            ^> Elegir "BouleCam Virtual Camera"
echo.
echo 🎙️ AUDIO (Microfono Nativo):
echo    En OBS: [+] Fuentes ^> "Dispositivo de captura de audio"
echo            ^> Elegir "CABLE Output (VB-Audio Virtual Cable)"
echo.
echo Ya no es necesario usar fuentes de Navegador!
echo.
pause
