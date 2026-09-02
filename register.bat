@echo off
echo ========================================================
echo        Registrando BouleCam Virtual Camera
echo ========================================================
echo.
echo NOTA: Este paso requiere permisos de Administrador.
echo.

if not exist "%~dp0build\Release\boulecam-vcam.dll" (
    echo [ERROR] No se encuentra '%~dp0build\Release\boulecam-vcam.dll'.
    echo Primero debes ejecutar 'build.bat' para compilar el proyecto.
    echo.
    pause
    exit /b 1
)

echo [1/3] Registrando DLL COM en Windows...
regsvr32.exe /s "%~dp0build\Release\boulecam-vcam.dll"

echo [2/3] Activando Virtual Camera en Media Foundation...
"%~dp0build\Release\register_vcam.exe" --install

echo [3/3] Configurando Reglas de Firewall de Windows (Puertos 8088, 8089 y 8090)...
netsh advfirewall firewall delete rule name="BouleCam Streaming" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Streaming" dir=in action=allow protocol=TCP localport=8088,8090 enable=yes >nul 2>&1
netsh advfirewall firewall delete rule name="BouleCam Discovery UDP" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Discovery UDP" dir=in action=allow protocol=UDP localport=8089 enable=yes >nul 2>&1
netsh advfirewall firewall delete rule name="BouleCam Desktop App" >nul 2>&1
netsh advfirewall firewall add rule name="BouleCam Desktop App" dir=in action=allow program="%~dp0build\Release\boulecam-desktop.exe" enable=yes >nul 2>&1

echo.
echo ========================================================
echo      CAMARA VIRTUAL Y FIREWALL INSTALADOS CON EXITO!
echo ========================================================
echo Ya aparece en Zoom, Google Meet, Teams, OBS, etc.
echo Conexiones Wi-Fi y USB habilitadas en Windows Firewall.
echo.
pause
