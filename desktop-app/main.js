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

app.whenReady().then(() => {
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
});

// No cerrar al cerrar ventanas, mantener en tray
app.on('window-all-closed', () => {
    // Mantener la app activa en el System Tray
});
