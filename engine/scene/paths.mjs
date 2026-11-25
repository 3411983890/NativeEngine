import path from 'node:path';

// Editable .screen scenes share the project's assets root. Imported previews
// and packaged scenes retain their existing scene-relative dependency layout.
export function sceneResourceRoot(scenePath, assetsDirectory) {
  const parent = path.dirname(path.resolve(scenePath));
  for (let directory = parent;; directory = path.dirname(directory)) {
    if (path.basename(directory) === '.screen') return path.resolve(assetsDirectory ?? path.join(path.dirname(directory), 'assets'));
    if (path.dirname(directory) === directory) return parent;
  }
}
