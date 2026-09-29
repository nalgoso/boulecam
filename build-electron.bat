@echo off
setlocal enabledelayedexpansion
title BouleCam - Compilador de Version Standalone Portable

echo ==========================================================
echo        BouleCam - Generador de Build Electron
echo ==========================================================
echo.

:: 1. Ir a la raiz del repositorio
cd /d "%~dp0"

:: 2. Obtener la version desde desktop-app\package.json usando node
for /f "usebackq tokens=*" %%a in (`node -e "try { console.log(require('./desktop-app/package.json').version); } catch(e) { process.exit(1); }"`) do (
    set "APP_VERSION=%%a"
)

if "%APP_VERSION%"=="" (
    echo [ERROR] No se pudo leer la version desde desktop-app\package.json.
    pause
    exit /b 1
)

echo [1/3] Version detectada en package.json: v%APP_VERSION%
echo.

:: 3. Compilar el motor C++ (Release) para asegurar que el binario integrado este actualizado
echo [2/3] Verificando y compilando el motor C++ (Release)...

set "MINGW_BIN=C:\Users\pablo\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20260616-ucrt-x86_64\bin"
if exist "%MINGW_BIN%\clang++.exe" (
    set "PATH=%MINGW_BIN%;%PATH%"
    if not exist "build-mingw" (
        cmake -B build-mingw -S . -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
    )
    cmake --build build-mingw
    if errorlevel 1 (
        echo [ERROR] Fallo la compilacion con MinGW.
        pause
        exit /b 1
    )
    if not exist "build\Release" mkdir "build\Release"
    copy /y "build-mingw\Release\boulecam-desktop.exe" "build\Release\boulecam-desktop.exe" >nul
    copy /y "build-mingw\Release\boulecam-vcam.dll" "build\Release\boulecam-vcam.dll" >nul
    copy /y "build-mingw\Release\register_vcam.exe" "build\Release\register_vcam.exe" >nul
) else (
    if not exist "build" (
        mkdir build
        cmake -B build -G "Visual Studio 17 2022" -A x64
    )
    cmake --build build --config Release
    if errorlevel 1 (
        echo [ERROR] Fallo la compilacion del motor C++ en build\Release.
        pause
        exit /b 1
    )
)
echo [OK] Motor C++ compilado en build\Release\boulecam-desktop.exe.
echo.

:: 4. Empaquetar ejecutable standalone portable con Electron
echo [3/3] Empaquetando ejecutable standalone portable de Electron...
cd desktop-app
if not exist "node_modules" (
    echo [INFO] Instalando dependencias de npm...
    call npm install
)

call npm run dist
if errorlevel 1 (
    echo.
    echo [ERROR] Fallo la generacion del empaquetado de Electron.
    cd ..
    pause
    exit /b 1
)
cd ..

:: 5. Comprobar resultado y copiar a la raiz
set "OUTPUT_EXE=desktop-app\dist\BouleCam-v%APP_VERSION%.exe"
if exist "%OUTPUT_EXE%" (
    copy /y "%OUTPUT_EXE%" "BouleCam-v%APP_VERSION%.exe" >nul
    if exist "BouleCam.exe" del /f /q "BouleCam.exe" >nul 2>&1
    if exist "desktop-app\dist\BouleCam.exe" del /f /q "desktop-app\dist\BouleCam.exe" >nul 2>&1
    echo.
    echo ==========================================================
    echo [EXITO] Ejecutable standalone generado correctamente:
    echo        %CD%\BouleCam-v%APP_VERSION%.exe
    echo ==========================================================
) else (
    echo.
    echo [AVISO] Compilacion finalizada. Revisa la carpeta desktop-app\dist\
)

echo.
pause
