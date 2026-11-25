import test from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { makeAnimationFixture } from '../tools/offline/animation-fixture.mjs';
import { importAnimation } from '../tools/offline/animation.mjs';

async function fixture(action, mutate = () => {}) {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'nativegpu-rig-'));
  try {
    const doc = makeAnimationFixture();
    mutate(doc);
    const file = path.join(dir, 'model.gltf');
    await fs.writeFile(file, JSON.stringify(doc));
    return await action(file, dir, doc);
  } finally {
    await fs.rm(dir, { recursive: true, force: true });
  }
}
const nodeIndex = (doc, name) => doc.nodes.findIndex(node => node.name === name);

test('a plain skeleton is diagnosed into a persistent biped rig with inferred contacts', async () => {
  await fixture(async file => {
    const a = await importAnimation(file), again = await importAnimation(file);
    const rig = a.header.rig;
    const boneIndex = name => a.header.bones.findIndex(bone => bone.name === name);
    assert.equal(rig.source, 'auto');
    assert.equal(rig.version, 2);
    assert.equal(rig.pelvis, boneIndex('hips'));
    assert.equal(rig.lookBone, boneIndex('head'));
    assert.equal(rig.legs.length, 2);
    assert.deepEqual(rig.legs.map(leg => leg.root), [boneIndex('left_thigh'), boneIndex('right_thigh')]);
    assert.ok(rig.confidence > .5, 'plain biped needs a usable confidence');
    assert.ok(rig.settings.height > 1.5 && rig.settings.height < 2.1);
    assert.ok(rig.settings.turnStepDuration > 0 && rig.settings.turnStepHeight > 0);
    for (const leg of rig.legs) {
      assert.ok(leg.soleHeight > 0 && leg.soleHeight < .3);
      assert.ok(leg.footLength > 0 && leg.footLength < .5);
      assert.ok(Math.hypot(...leg.pole) > .01, 'knee pole must leave the leg axis');
    }
    // Arms are the chains the generic contact layer drives for a hand on a rail.
    // They are detected from the same skeleton pass and must point at the hand.
    assert.equal(rig.arms.length, 2);
    assert.deepEqual(rig.arms.map(chain => chain.root), [boneIndex('left_arm'), boneIndex('right_arm')]);
    assert.deepEqual(rig.arms.map(chain => chain.tip), [boneIndex('left_hand'), boneIndex('right_hand')]);
    for (const chain of rig.arms) {
      assert.equal(a.header.bones[chain.joint].parent, chain.root);
      assert.equal(a.header.bones[chain.tip].parent, chain.joint);
      assert.ok(Math.hypot(...chain.pole) > .01, 'elbow pole must leave the arm axis');
      assert.ok(chain.pole[2] > 0, 'a straight rest arm bends backwards');
    }
    const idle = a.header.clips.find(clip => clip.name === 'idle');
    assert.equal(idle.contactSource, 'inferred');
    assert.deepEqual(idle.contacts, [[0, 1], [0, 1]]);
    assert.ok(idle.contactConfidence > .5);
    const walk = a.header.clips.find(clip => clip.name === 'walk');
    assert.equal(walk.contactSource, 'inferred');
    // The fixture's two foot arcs dip at nearly the same phase, so a
    // conservative inference must stay below the runtime trust threshold
    // instead of inventing an alternating gait.
    assert.ok(walk.contactConfidence <= .5, 'ambiguous arcs must not be trusted');
    assert.ok(Number.isFinite(walk.locomotionSpeed ?? 0) && (walk.locomotionSpeed ?? 0) >= 0);
    assert.ok(a.header.clips.every(clip => Number.isFinite(clip.contactConfidence) && clip.contactConfidence >= 0 && clip.contactConfidence <= 1));
    assert.equal(a.text, again.text, 'diagnosis must be deterministic');
    assert.deepEqual(a.binary, again.binary);
  });
});

test('the bundled robot sample resolves a usable alternating gait', async t => {
  const source = path.resolve('assets/source/builtin/robot.glb');
  try {
    await fs.access(source);
  } catch {
    t.skip('bundled robot source is not present');
    return;
  }
  const a = await importAnimation(source, { normalize: true });
  const rig = a.header.rig;
  assert.equal(rig.source, 'auto');
  assert.equal(rig.legs.length, 2);
  assert.equal(rig.legs.filter(leg => leg.toe).length, 2);
  assert.ok(rig.confidence > .5);
  const walk = a.header.clips.find(clip => /walk/i.test(clip.name));
  const run = a.header.clips.find(clip => /run/i.test(clip.name));
  assert.equal(walk.contactSource, 'inferred');
  assert.ok(walk.contactConfidence >= .5, 'Mixamo-style walk must be usable');
  assert.ok(run.contactConfidence >= .5, 'Mixamo-style run must be usable');
  assert.ok(walk.locomotionSpeed > .05, 'walk must report a support speed');
  assert.ok(run.locomotionSpeed > walk.locomotionSpeed, 'run must be faster than walk');
});

test('an authored rig wins and still receives diagnosed foot proportions', async () => {
  await fixture(async file => {
    const a = await importAnimation(file);
    assert.equal(a.header.rig.source, 'authored');
    assert.equal(a.header.rig.confidence, 1);
    assert.equal(a.header.rig.legs.length, 2);
    assert.ok(a.header.rig.legs.every(leg => leg.soleHeight === .11));
    assert.ok(a.header.rig.legs.every(leg => Math.hypot(...leg.pole) > .001), 'authored chain without a pole must inherit the diagnosed one');
  }, doc => {
    doc.extras = { nativegpu: { rig: { pelvis: 'hips', lookBone: 'head', legs: [['left_thigh', 'left_calf', 'left_foot'], ['right_thigh', 'right_calf', 'right_foot']].map(([root, joint, tip]) => ({ root, joint, tip, soleHeight: .11 })) } } };
  });
});

test('ambiguous or unsupported skeletons stay explicit instead of guessing', async () => {
  await fixture(async file => {
    const a = await importAnimation(file);
    assert.equal(a.header.rig.legs.length, 0);
    assert.ok(a.header.rig.diagnostics.ambiguous.length > 0);
    assert.ok(a.header.rig.diagnostics.warnings.length > 0);
    assert.ok(a.header.rig.confidence < .5);
    assert.ok((a.header.importWarnings ?? []).length > 0);
  }, doc => { doc.nodes[nodeIndex(doc, 'left_calf')].name = 'left_upper_leg'; });
  await fixture(async file => {
    const a = await importAnimation(file);
    assert.equal(a.header.rig.legs.length, 0);
    assert.ok(a.header.rig.diagnostics.warnings.some(warning => /hierarchy/i.test(warning)));
  }, doc => {
    doc.nodes[nodeIndex(doc, 'left_calf')].children = doc.nodes[nodeIndex(doc, 'left_calf')].children.filter(child => child !== nodeIndex(doc, 'left_foot'));
    doc.nodes[nodeIndex(doc, 'left_thigh')].children.push(nodeIndex(doc, 'left_foot'));
    doc.nodes[nodeIndex(doc, 'left_thigh')].children.sort((a, b) => a - b);
  });
  await fixture(async file => {
    const a = await importAnimation(file);
    assert.equal(a.header.rig.legs.length, 0);
    assert.ok(a.header.rig.diagnostics.missing.length > 0);
  }, doc => { doc.nodes[nodeIndex(doc, 'right_foot')].name = 'right_piston'; });
});

test('contact inference reports planted non-loop clips without inventing a gait', async () => {
  await fixture(async file => {
    const a = await importAnimation(file);
    for (const clip of a.header.clips) {
      assert.ok(['authored', 'inferred', 'unknown'].includes(clip.contactSource));
      if (clip.contactSource === 'inferred' && clip.contactConfidence >= .5)
        assert.deepEqual(clip.contacts.length, 2);
    }
    const interact = a.header.clips.find(clip => clip.name === 'interact');
    assert.ok(interact.contactConfidence <= 1);
  });
});