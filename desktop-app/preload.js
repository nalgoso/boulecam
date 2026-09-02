const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('boulecam', {
    platform: process.platform,
    version: '1.0.0'
});
