const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell } = require('electron');
const path = require('path');
const { spawn, exec } = require('child_process');
const fs = require('fs');

let mainWindow = null;
let serviceProcess = null;
let tray = null;
let isQuitting = false;
let balloonShown = false;
let hasClearedVersionCache = false;

// 1. Instancia única: Evitar abrir múltiples copias de la aplicación
const gotTheLock = app.requestSingleInstanceLock();
if (!gotTheLock) {
    app.quit();
} else {
    app.on('second-instance', () => {
        if (mainWindow) {
            if (mainWindow.isMinimized()) mainWindow.restore();
            mainWindow.show();
            mainWindow.focus();
        }
    });
}

function checkAndCleanVersionCache() {
    const appData = process.env.APPDATA || (process.env.USERPROFILE ? path.join(process.env.USERPROFILE, 'AppData', 'Roaming') : null);
    if (!appData) return;

    const bouleCamDir = path.join(appData, 'BouleCam');
    try {
        if (!fs.existsSync(bouleCamDir)) {
            fs.mkdirSync(bouleCamDir, { recursive: true });
        }
    } catch (e) {}

    const versionFile = path.join(bouleCamDir, 'version.txt');
    const currentVersion = app.getVersion();
    let isNewVersion = false;

    try {
        if (!fs.existsSync(versionFile)) {
            isNewVersion = true;
        } else {
            const savedVer = fs.readFileSync(versionFile, 'utf8').trim();
            if (savedVer !== currentVersion) {
                isNewVersion = true;
            }
        }
    } catch (e) {
        isNewVersion = true;
    }

    if (isNewVersion) {
        console.log(`[Main] Nueva versión detectada (${currentVersion}). Limpiando cachés anteriores y dispositivos bloqueados...`);
        hasClearedVersionCache = true;

        // 1. Limpiar archivo de cámaras bloqueadas
        const locksFile = path.join(bouleCamDir, 'locked_cameras.json');
        if (fs.existsSync(locksFile)) {
            try {
                fs.unlinkSync(locksFile);
                console.log('[Main] locked_cameras.json eliminado exitosamente.');
            } catch (e) {
                console.warn('[Main] No se pudo eliminar locked_cameras.json:', e.message);
            }
        }

        // 2. Guardar versión actual
        try {
            fs.writeFileSync(versionFile, currentVersion, 'utf8');
        } catch (e) {}
    }
}

function prepareBinaries() {
    let binDir = path.join(__dirname, '..', 'build', 'Release');
    if (app.isPackaged) {
        const bundledInResources = path.join(process.resourcesPath, 'bin');
        if (fs.existsSync(bundledInResources)) {
            binDir = bundledInResources;
        } else {
            binDir = process.resourcesPath;
        }
    }

    const appData = process.env.APPDATA || (process.env.USERPROFILE ? path.join(process.env.USERPROFILE, 'AppData', 'Roaming') : null);
    if (!appData) return binDir;

    const permanentBin = path.join(appData, 'BouleCam', 'bin');
    try {
        if (!fs.existsSync(permanentBin)) {
            fs.mkdirSync(permanentBin, { recursive: true });
        }
        const filesToCopy = ['boulecam-desktop.exe', 'boulecam-vcam.dll', 'register_vcam.exe'];
        for (const file of filesToCopy) {
            const src = path.join(binDir, file);
            const dst = path.join(permanentBin, file);
            if (fs.existsSync(src)) {
                try {
                    fs.copyFileSync(src, dst);
                } catch (e) {
                    console.warn(`[Binaries] Copia omitida (archivo en uso): ${file}`);
                }
            }
        }
        return permanentBin;
    } catch (e) {
        console.warn('[Binaries] Warning copying to AppData:', e.message);
        return binDir;
    }
}

function ensureFirewallRules(exePath) {
    if (process.platform !== 'win32') return;

    // Registrar reglas de Firewall en Windows para puertos TCP 8088/8090, UDP 8089 y el binario ejecutable
    const commands = [
        'netsh advfirewall firewall add rule name="BouleCam Streaming" dir=in action=allow protocol=TCP localport=8088,8090 profile=any enable=yes',
        'netsh advfirewall firewall add rule name="BouleCam Discovery UDP" dir=in action=allow protocol=UDP localport=8089 profile=any enable=yes',
        `netsh advfirewall firewall add rule name="BouleCam Desktop Binary" dir=in action=allow program="${exePath}" profile=any enable=yes`
    ];

    commands.forEach(cmd => {
        exec(cmd, () => {});
    });
}

function getServiceExePath() {
    const appData = process.env.APPDATA || (process.env.USERPROFILE ? path.join(process.env.USERPROFILE, 'AppData', 'Roaming') : null);
    if (appData) {
        const permanentExe = path.join(appData, 'BouleCam', 'bin', 'boulecam-desktop.exe');
        if (fs.existsSync(permanentExe)) return permanentExe;
    }

    if (app.isPackaged) {
        const bundledInResources = path.join(process.resourcesPath, 'bin', 'boulecam-desktop.exe');
        if (fs.existsSync(bundledInResources)) return bundledInResources;

        const nextToResources = path.join(process.resourcesPath, 'boulecam-desktop.exe');
        if (fs.existsSync(nextToResources)) return nextToResources;

        const nextToExe = path.join(path.dirname(app.getPath('exe')), 'boulecam-desktop.exe');
        if (fs.existsSync(nextToExe)) return nextToExe;
    }
    return path.join(__dirname, '..', 'build', 'Release', 'boulecam-desktop.exe');
}

function startBackendService() {
    const exePath = getServiceExePath();
    if (fs.existsSync(exePath)) {
        try {
            // Clean up any stale orphan instance before launching
            const { execSync } = require('child_process');
            execSync('taskkill /F /IM boulecam-desktop.exe >nul 2>&1');
        } catch (ignored) {}

        ensureFirewallRules(exePath);

        console.log('[Main] Iniciando motor C++ en segundo plano:', exePath);
        try {
            serviceProcess = spawn(exePath, [], {
                cwd: path.dirname(exePath),
                windowsHide: true,
                detached: false,
                stdio: ['ignore', 'pipe', 'pipe']
            });

            serviceProcess.stdout.on('data', (d) => {
                process.stdout.write(`[Motor C++]: ${d}`);
            });

            serviceProcess.stderr.on('data', (d) => {
                process.stderr.write(`[Motor C++ Error]: ${d}`);
            });

            serviceProcess.on('exit', (code) => {
                console.log(`[Main] El motor C++ finalizó con código ${code}`);
                serviceProcess = null;
            });
        } catch (e) {
            console.warn('[Main] Aviso del motor:', e.message);
        }
    } else {
        console.error('[Main] No se encontró el ejecutable del motor en:', exePath);
    }
}

function createTray() {
    if (tray) return;

    const iconPath = path.join(__dirname, 'assets', 'icon.png');
    let trayIcon;
    try {
        trayIcon = nativeImage.createFromPath(iconPath).resize({ width: 16, height: 16 });
    } catch (e) {
        trayIcon = nativeImage.createEmpty();
    }

    tray = new Tray(trayIcon);
    tray.setToolTip('BouleCam - Iglesia Boulevard Guzmán');

    const contextMenu = Menu.buildFromTemplate([
        {
            label: 'Abrir BouleCam',
            click: () => {
                if (mainWindow) {
                    mainWindow.show();
                    mainWindow.focus();
                }
            }
        },
        { type: 'separator' },
        {
            label: 'Sitio Web Oficial',
            click: async () => {
                await shell.openExternal('https://iglesiaboulevardguzman.com.ar');
            }
        },
        { type: 'separator' },
        {
            label: 'Cerrar BouleCam por completo',
            click: () => {
                isQuitting = true;
                app.quit();
            }
        }
    ]);

    tray.setContextMenu(contextMenu);

    // Clic en el icono del tray: restaurar o mostrar la ventana
    tray.on('click', () => {
        if (mainWindow) {
            if (mainWindow.isVisible()) {
                if (mainWindow.isMinimized()) mainWindow.restore();
                mainWindow.focus();
            } else {
                mainWindow.show();
                mainWindow.focus();
            }
        }
    });

    tray.on('double-click', () => {
        if (mainWindow) {
            if (mainWindow.isMinimized()) mainWindow.restore();
            mainWindow.show();
            mainWindow.focus();
        }
    });
}

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 1100,
        height: 780,
        minWidth: 950,
        minHeight: 680,
        title: "BouleCam",
        icon: path.join(__dirname, 'assets', 'icon.png'),
        backgroundColor: '#0a0a0f',
        webPreferences: {
            nodeIntegration: false,
            contextIsolation: true,
            preload: path.join(__dirname, 'preload.js')
        },
        autoHideMenuBar: true
    });

    mainWindow.loadFile('index.html');

    if (hasClearedVersionCache) {
        mainWindow.webContents.session.clearStorageData().catch(() => {});
        mainWindow.webContents.session.clearCache().catch(() => {});
        console.log('[Main] Caché web y almacenamiento local limpiados por actualización de versión.');
    }

    // Evitar cierre accidental: Si el usuario presiona la X o cierra desde la barra de tareas,
    // se oculta al System Tray (área de notificaciones) para proteger la transmisión en vivo.
    mainWindow.on('close', (event) => {
        if (!isQuitting) {
            event.preventDefault();
            mainWindow.hide();

            // Notificación informativa solo la primera vez que se oculta
            if (tray && !balloonShown) {
                balloonShown = true;
                tray.displayBalloon({
                    title: 'BouleCam activa en segundo plano',
                    content: 'BouleCam sigue funcionando para tu transmisión. Puedes abrirla desde los iconos ocultos junto al reloj.',
                    iconType: 'info'
                });
            }
            return false;
        }
    });

    mainWindow.on('closed', () => {
        mainWindow = null;
    });
}

function autoRegisterVirtualCamera() {
    let binDir = path.join(__dirname, '..', 'build', 'Release');
    if (app.isPackaged) {
        const bundledInResources = path.join(process.resourcesPath, 'bin');
        if (fs.existsSync(bundledInResources)) {
            binDir = bundledInResources;
        } else {
            binDir = process.resourcesPath;
        }
    }

    // Ensure permanent user directory in APPDATA so OBS Studio can always load the DLL across updates and reboots
    const appData = process.env.APPDATA || (process.env.USERPROFILE ? path.join(process.env.USERPROFILE, 'AppData', 'Roaming') : null);
    let targetDir = binDir;
    if (appData) {
        const permanentBin = path.join(appData, 'BouleCam', 'bin');
        try {
            if (!fs.existsSync(permanentBin)) {
                fs.mkdirSync(permanentBin, { recursive: true });
            }
            const srcDll = path.join(binDir, 'boulecam-vcam.dll');
            const dstDll = path.join(permanentBin, 'boulecam-vcam.dll');
            if (fs.existsSync(srcDll)) {
                fs.copyFileSync(srcDll, dstDll);
            }
            const srcReg = path.join(binDir, 'register_vcam.exe');
            const dstReg = path.join(permanentBin, 'register_vcam.exe');
            if (fs.existsSync(srcReg)) {
                fs.copyFileSync(srcReg, dstReg);
            }
            if (fs.existsSync(dstDll)) {
                targetDir = permanentBin;
            }
        } catch (e) {
            console.warn('[VCam] Warning copying to AppData:', e.message);
        }
    }

    const vcamDll = path.join(targetDir, 'boulecam-vcam.dll');
    const regExe = path.join(targetDir, 'register_vcam.exe');

    const cleanDuplicateKeys = () => {
        exec('reg delete "HKCU\\Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\BouleCam Virtual Camera" /f', () => {});
        exec('reg delete "HKCU\\Software\\Classes\\CLSID\\{33D9A762-90C8-11d0-BD43-00A0C911CE86}\\Instance\\BouleCam Audio" /f', () => {});
    };

    cleanDuplicateKeys();

    if (fs.existsSync(regExe)) {
        exec(`"${regExe}" --install`, (err, stdout) => {
            cleanDuplicateKeys();
            if (err) console.warn('[VCam] DirectShow reg warning:', err.message);
            else console.log('[VCam] DirectShow Virtual Camera & Audio registrados exitosamente en Windows.');
        });
    } else if (fs.existsSync(vcamDll)) {
        exec(`regsvr32.exe /s "${vcamDll}"`, (err) => {
            cleanDuplicateKeys();
            if (err) console.warn('[VCam] COM reg warning:', err.message);
            else console.log('[VCam] Servidor COM registrado silenciosamente.');
        });
    }
}

app.whenReady().then(() => {
    checkAndCleanVersionCache();
    prepareBinaries();
    autoRegisterVirtualCamera();
    startBackendService();
    createWindow();
    createTray();

    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) {
            createWindow();
        } else if (mainWindow) {
            mainWindow.show();
            mainWindow.focus();
        }
    });
});

app.on('before-quit', () => {
    isQuitting = true;
});

app.on('will-quit', () => {
    if (tray) {
        tray.destroy();
        tray = null;
    }
    if (serviceProcess) {
        try {
            console.log('[Main] Deteniendo motor C++...');
            exec(`taskkill /F /PID ${serviceProcess.pid} >nul 2>&1`);
            serviceProcess.kill();
        } catch (ignored) {}
    }
    try {
        exec('taskkill /F /IM boulecam-desktop.exe >nul 2>&1');
    } catch (ignored) {}
});

// No cerrar al cerrar ventanas, mantener en tray
app.on('window-all-closed', () => {
    // Mantener la app activa en el System Tray
});
