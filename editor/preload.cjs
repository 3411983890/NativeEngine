// Preload for the editor window. Two capabilities are exposed, both narrow:
//   - move/hide the native viewport child window;
//   - send one structured command to it and subscribe to its acknowledgements.
// The page cannot write the pipe directly, cannot name a path the main process has not
// sanitised, and cannot read the filesystem or spawn processes.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('forgeViewport', {
  // rect: { x, y, width, height, visible } in physical pixels, relative to the client area.
  apply: (rect) => ipcRenderer.send('forge:viewport', rect),
  // command: { command: 'load'|'view'|'capture'|'status', species?, lod?, revision?, digest?, debug?, path? }
  command: (command) => ipcRenderer.invoke('forge:viewport-command', command),
  // The host's last acknowledged state. Resolves to null outside Electron.
  state: () => ipcRenderer.invoke('forge:viewport-state'),
  // Rebind the native child process to a managed project folder.
  setProject: (folder) => ipcRenderer.invoke('forge:viewport-project', folder),
  // cb(state) on every acknowledgement: { species, lod, revision, digest, debug, ok, error }
  onAck: (cb) => {
    const listener = (_event, state) => cb(state);
    ipcRenderer.on('forge:viewport-ack', listener);
    return () => ipcRenderer.removeListener('forge:viewport-ack', listener);
  },
});
