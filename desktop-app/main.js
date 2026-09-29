const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell } = require('electron');
const path = require('path');
const { spawn, exec } = require('child_process');
const fs = require('fs');

let mainWindow = null;
let serviceProcess = null;
let tray = null;
let isQuitting = false;
let balloonShown = false;

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

function getServiceExePath() {
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

    mainWindow.on('close', (event) => {
        isQuitting = true;
        app.quit();
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

app.on('window-all-closed', () => {
    isQuitting = true;
    app.quit();
});
