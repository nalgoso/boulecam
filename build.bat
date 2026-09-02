@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo          Compilador Automatico para BouleCam
echo ========================================================
echo.

:: Detectar y agregar CMake al PATH si esta instalado en rutas estandar
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files\CMake\bin;!PATH!"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
)

where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] No se encontro 'cmake' en el sistema.
    echo Asegurate de reiniciar tu terminal si acabas de instalarlo o agregalo a tu PATH.
    echo.
    pause
    exit /b 1
)

:: 2. Crear carpeta build si no existe
if not exist "build" (
    echo Creando carpeta 'build'...
    mkdir build
)

:: 3. Generar solucion de Visual Studio 2022
echo.
echo [1/2] Generando proyecto con CMake (Visual Studio 2022 x64)...
cmake -B build -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% NEQ 0 (
    echo [Info] Limpiando cache antigua y reintentando...
    if exist "build\CMakeCache.txt" del /f /q "build\CMakeCache.txt"
    cmake -B build -G "Visual Studio 17 2022" -A x64
    if %ERRORLEVEL% NEQ 0 (
        echo [ERROR] Fallo al generar el proyecto con CMake.
        pause
        exit /b 1
    )
)

:: 4. Compilar en modo Release
echo.
echo [2/2] Compilando binarios en Release...
cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Ocurrio un error durante la compilacion.
    pause
    exit /b 1
)

echo.
echo ========================================================
echo      COMPILACION COMPLETADA CON EXITO!
echo ========================================================
echo Los ejecutables estan listos en la carpeta 'build\Release':
echo   - boulecam-desktop.exe  (Servidor receptor)
echo   - boulecam-vcam.dll     (Driver de camara virtual)
echo   - register_vcam.exe     (Instalador de la camara)
echo.
echo Siguiente paso: Ejecuta 'register.bat' como Administrador para activar la camara.
echo.
pause
