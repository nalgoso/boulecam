const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('boulecam', {
    platform: process.platform,
    version: '1.0.0',
    getDisplays: () => ipcRenderer.invoke('get-displays'),
    toggleProjector: (data) => ipcRenderer.invoke('toggle-projector', data),
    getProjectorState: () => ipcRenderer.invoke('get-projector-state'),
    onProjectorStateChanged: (callback) => {
        ipcRenderer.on('projector-state-changed', (event, state) => callback(state));
    }
});
