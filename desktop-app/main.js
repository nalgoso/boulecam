const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell, screen } = require('electron');
const path = require('path');
const { spawn, exec } = require('child_process');
const fs = require('fs');

let mainWindow = null;
let projectorWindow = null;
let currentProjectorDisplayId = null;
let serviceProcess = null;
let tray = null;
let isQuitting = false;
let balloonShown = false;
let hasClearedVersionCache = false;

// Evitar que Chromium/Electron suspenda o limite rendimiento cuando BouleCam corre en segundo plano o minimizada
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');

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
        console.log(`[Main] Nueva versión detectada (${currentVersion}). Actualizando caché de interfaz...`);
        hasClearedVersionCache = true;

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
        // Ensure background processes are terminated so new version files can overwrite cleanly
        try {
            const { execSync } = require('child_process');
            execSync('taskkill /F /IM boulecam-desktop.exe >nul 2>&1');
        } catch (ignored) {}

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

    // Registrar reglas de Firewall en Windows para puertos TCP 8088/8095, UDP 8089 y el binario ejecutable
    const commands = [
        'netsh advfirewall firewall add rule name="BouleCam Streaming" dir=in action=allow protocol=TCP localport=8088,8095 profile=any enable=yes',
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
    tray.setToolTip('BouleCam');

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
            backgroundThrottling: false,
            preload: path.join(__dirname, 'preload.js')
        },
        autoHideMenuBar: true
    });

    mainWindow.loadFile('index.html');

    if (hasClearedVersionCache) {
        mainWindow.webContents.session.clearCache().catch(() => {});
        console.log('[Main] Caché web limpiada por actualización de versión.');
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

function notifyProjectorState() {
    if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('projector-state-changed', {
            active: !!projectorWindow,
            displayId: currentProjectorDisplayId
        });
    }
}

ipcMain.handle('get-displays', () => {
    try {
        const primary = screen.getPrimaryDisplay();
        const all = screen.getAllDisplays();
        return all.map((d, index) => {
            const isPrimary = d.id === primary.id;
            const rawLabel = (d.label && d.label.trim()) ? d.label.trim() : '';
            const isGeneric = !rawLabel || rawLabel === 'Generic PnP Monitor' || rawLabel === 'Monitor PnP genérico';
            const displayName = !isGeneric ? rawLabel : `Pantalla ${index + 1}`;
            return {
                id: d.id,
                name: displayName,
                screenNumber: index + 1,
                resolution: `${d.bounds.width}×${d.bounds.height}`,
                isPrimary: isPrimary,
                bounds: d.bounds
            };
        });
    } catch (e) {
        console.error('[Projector] Error enumerating displays:', e);
        return [];
    }
});

ipcMain.handle('get-projector-state', () => {
    return {
        active: !!projectorWindow,
        displayId: currentProjectorDisplayId
    };
});

ipcMain.handle('toggle-projector', (event, { displayId, camId }) => {
    try {
        if (projectorWindow) {
            // Si ya está proyectando en esta misma pantalla, alternar/cerrar
            if (currentProjectorDisplayId === displayId) {
                projectorWindow.close();
                projectorWindow = null;
                currentProjectorDisplayId = null;
                notifyProjectorState();
                return { active: false };
            }
        }

        const displays = screen.getAllDisplays();
        const target = displays.find(d => d.id === displayId) || displays[0];
        if (!target) return { active: false };

        // Usar cam=0 (señal activa en BouleCam) para permitir cambiar entre múltiples cámaras en vivo sin recargar el proyector
        const streamUrl = `http://127.0.0.1:8095/obs/0`;

        if (!projectorWindow) {
            projectorWindow = new BrowserWindow({
                x: target.bounds.x,
                y: target.bounds.y,
                width: target.bounds.width,
                height: target.bounds.height,
                fullscreen: true,
                frame: false,
                autoHideMenuBar: true,
                alwaysOnTop: false,
                backgroundColor: '#000000',
                webPreferences: {
                    nodeIntegration: false,
                    contextIsolation: true,
                    backgroundThrottling: false
                }
            });

            projectorWindow.on('closed', () => {
                projectorWindow = null;
                currentProjectorDisplayId = null;
                notifyProjectorState();
            });

            // Tecla ESC para salir de pantalla completa
            projectorWindow.webContents.on('before-input-event', (event, input) => {
                if (input.key === 'Escape') {
                    if (projectorWindow) projectorWindow.close();
                }
            });

            // Ocultar cursor al estar inactivo y doble clic para salir
            projectorWindow.webContents.on('did-finish-load', () => {
                projectorWindow.webContents.executeJavaScript(`
                    let mouseTimer;
                    document.addEventListener('mousemove', () => {
                        document.body.style.cursor = 'default';
                        clearTimeout(mouseTimer);
                        mouseTimer = setTimeout(() => { document.body.style.cursor = 'none'; }, 2000);
                    });
                    document.addEventListener('dblclick', () => {
                        window.close();
                    });
                `).catch(() => {});
            });
        } else {
            // Mover a la pantalla seleccionada
            projectorWindow.setBounds(target.bounds);
            projectorWindow.setFullScreen(true);
        }

        currentProjectorDisplayId = target.id;
        projectorWindow.loadURL(streamUrl);
        projectorWindow.show();
        notifyProjectorState();
        return { active: true, displayId: currentProjectorDisplayId };
    } catch (e) {
        console.error('[Projector] Error toggling projector:', e);
        return { active: false, error: e.message };
    }
});

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
        const hives = ['HKCU', 'HKLM'];
        const vCat = '{860BB310-5D01-11d0-BD3B-00A0C911CE86}';
        const aCat = '{33D9A762-90C8-11d0-BD43-00A0C911CE86}';

        // Eliminar claves heredadas por nombre en HKCU y HKLM
        const staleVideo = [
            'BouleCam Virtual Camera',
            'BouleCam Virtual Camera 2',
            'BouleCam Virtual Camera 3',
            'BouleCam Virtual Camera 4',
            'BouleCam Virtual Camera (Media Foundation)',
            'DirectShow Softcam',
            '{6B47C030-85A4-4D6C-9A52-2A1E7F19D3B1}'
        ];
        const staleAudio = [
            'BouleCam Audio',
            'BouleCam Audio 2',
            'BouleCam Audio 3',
            'BouleCam Audio 4'
        ];

        hives.forEach(h => {
            staleVideo.forEach(inst => exec(`reg delete "${h}\\Software\\Classes\\CLSID\\${vCat}\\Instance\\${inst}" /f`, () => {}));
            staleAudio.forEach(inst => exec(`reg delete "${h}\\Software\\Classes\\CLSID\\${aCat}\\Instance\\${inst}" /f`, () => {}));
        });

        // Eliminar instancias estáticas antiguas en HKLM para que solo HKCU (gestionado dinámicamente por boulecam-desktop.exe) controle las cámaras visibles
        const dynamicVideoClsids = [
            '{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C011-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C012-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C013-85A4-4D6C-9A52-2A1E7F19D3B1}'
        ];
        const dynamicAudioClsids = [
            '{6B47C020-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C021-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C022-85A4-4D6C-9A52-2A1E7F19D3B1}',
            '{6B47C023-85A4-4D6C-9A52-2A1E7F19D3B1}'
        ];
        dynamicVideoClsids.forEach(clsid => exec(`reg delete "HKLM\\Software\\Classes\\CLSID\\${vCat}\\Instance\\${clsid}" /f`, () => {}));
        dynamicAudioClsids.forEach(clsid => exec(`reg delete "HKLM\\Software\\Classes\\CLSID\\${aCat}\\Instance\\${clsid}" /f`, () => {}));
    };

    try {
        const { execSync } = require('child_process');
        if (fs.existsSync(regExe)) {
            execSync(`"${regExe}" --install`, { windowsHide: true });
            console.log('[VCam] DirectShow Virtual Camera & Audio COM servers registrados.');
        } else if (fs.existsSync(vcamDll)) {
            execSync(`regsvr32.exe /s "${vcamDll}"`, { windowsHide: true });
        }
    } catch (err) {
        console.warn('[VCam] Aviso al registrar servidor COM:', err.message);
    }
    cleanDuplicateKeys();
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
    if (projectorWindow && !projectorWindow.isDestroyed()) {
        try { projectorWindow.close(); } catch (ignored) {}
        projectorWindow = null;
    }
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
