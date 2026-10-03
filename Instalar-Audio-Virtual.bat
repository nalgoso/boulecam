@echo off
setlocal enabledelayedexpansion
title Instalador de Audio Virtual BouleCam (VB-Audio Virtual Cable)

echo ========================================================
echo     Instalador de Cable de Audio Virtual para BouleCam
echo      (Microfono aislado para OBS Studio y Windows)
echo ========================================================
echo.

:: 1. Verificar elevacion de Administrador
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [INFO] Se requieren permisos de Administrador para instalar el driver de audio.
    echo Solicitando elevacion UAC...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

:: 2. Verificar si ya esta instalado
echo [1/3] Verificando controladores de audio actuales...
powershell -Command "Get-CimInstance Win32_SoundDevice | Where-Object { $_.Name -match 'CABLE|VB-Audio' } | Select-Object -First 1 Name" > "%TEMP%\vbcable_check.txt" 2>nul
set /p INSTALLED=<"%TEMP%\vbcable_check.txt"
del "%TEMP%\vbcable_check.txt" 2>nul

if not "!INSTALLED!"=="" (
    echo [OK] El Cable de Audio Virtual ya se encuentra instalado: !INSTALLED!
    echo BouleCam enlazara automaticamente el microfono a este dispositivo.
    echo.
    echo En OBS Studio, puedes seleccionarlo en:
    echo [+] Fuentes ^> Captura de entrada de audio ^> "CABLE Output"
    echo.
    pause
    exit /b 0
)

:: 3. Descargar driver oficial firmado de VB-Audio
echo [2/3] Descargando VB-Audio Virtual Cable (Driver firmado por Microsoft WHQL)...
set "ZIP_PATH=%TEMP%\VBCABLE_Driver_Pack43.zip"
set "EXTRACT_DIR=%TEMP%\vbcable_setup"

powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; (New-Object Net.WebClient).DownloadFile('https://download.vb-audio.com/Download_VAC/VBCABLE_Driver_Pack43.zip', '%ZIP_PATH%')"
if not exist "%ZIP_PATH%" (
    echo [ERROR] No se pudo descargar el paquete del controlador.
    echo Puedes descargarlo manualmente desde: https://vb-audio.com/Cable/
    pause
    exit /b 1
)

echo [3/3] Descomprimiendo e instalando...
if exist "%EXTRACT_DIR%" rd /s /q "%EXTRACT_DIR%" 2>nul
powershell -Command "Expand-Archive -LiteralPath '%ZIP_PATH%' -DestinationPath '%EXTRACT_DIR%' -Force"

if exist "%EXTRACT_DIR%\VBCABLE_Setup_x64.exe" (
    echo Ejecutando instalador silencioso...
    cd /d "%EXTRACT_DIR%"
    start /wait VBCABLE_Setup_x64.exe -i -h
) else if exist "%EXTRACT_DIR%\VBCABLE_Setup.exe" (
    cd /d "%EXTRACT_DIR%"
    start /wait VBCABLE_Setup.exe -i -h
) else (
    echo [ERROR] No se encontro el ejecutable de instalacion en el archivo extraido.
    pause
    exit /b 1
)

:: Limpiar archivos temporales
del "%ZIP_PATH%" 2>nul
rd /s /q "%EXTRACT_DIR%" 2>nul

echo.
echo ========================================================
echo     INSTALACION COMPLETADA CON EXITO!
echo ========================================================
echo.
echo 🎙️ En OBS Studio:
echo    1. Ve a [+] Fuentes ^> "Captura de entrada de audio"
echo    2. Elige "CABLE Output (VB-Audio Virtual Cable)"
echo.
echo El microfono del telefono ingresara de forma 100% aislada
echo sin reproducirse en tus altavoces ni mezclarse con el audio de PC.
echo.
pause
