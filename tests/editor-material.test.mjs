import test from 'node:test';
import assert from 'node:assert/strict';
import { createEditorService } from '../editor/service.mjs';

test('editor material_patch previews a named material without writing', async () => {
  const service = createEditorService({ root: process.cwd() });
  try {
    const response = await service.call('material_patch', {
      scenePath: '.screen/town.scene.json',
      material: 'cobble',
      changes: { wetness: 0.55 },
    });
    assert.equal(response.ok, true);
    assert.equal(response.result.applied, false);
    assert.equal(response.result.after.wetness, 0.55);
    assert.ok(response.result.affectedEntities >= 1);
    assert.ok(response.result.textureBudget.bytes > 0);
  } finally {
    await service.close();
  }
});
