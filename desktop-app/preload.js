const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('boulecam', {
    platform: process.platform,
    version: '1.0.0',
    installVcam: () => ipcRenderer.invoke('install-vcam'),
    uninstallVcam: () => ipcRenderer.invoke('uninstall-vcam')
});
