import {requireAsset} from './gltf-document.mjs';

export const AUDIO_BYTES_LIMIT=64*1024*1024;
export const AUDIO_SCENE_BYTES_LIMIT=256*1024*1024;
// NGS1 v1: interleaved finite float32, 48 kHz, mono or stereo. Read exactly
// the bytes covered by the scene reference, never reopen a decoded dependency.
export function inspectNgsnd(bytes) {
  requireAsset(bytes.length>=44&&bytes.length<=AUDIO_BYTES_LIMIT&&bytes.toString('ascii',0,4)==='NGS1','Invalid or oversized sound payload','AUDIO');
  const version=bytes.readUInt16LE(4),channels=bytes.readUInt16LE(6),sampleRate=bytes.readUInt32LE(8),frames=bytes.readUInt32LE(12),format=bytes.readUInt16LE(16),flags=bytes.readUInt16LE(18),offset=bytes.readUInt32LE(36),size=bytes.readUInt32LE(40);
  requireAsset(version===1&&(channels===1||channels===2)&&sampleRate===48000&&frames>0&&format===2&&flags<=1&&offset>=44&&offset%4===0&&size===frames*channels*4&&offset+size===bytes.length,'Unsupported or truncated sound payload','AUDIO');
  const loopStart=bytes.readUInt32LE(20),loopEnd=bytes.readUInt32LE(24);
  requireAsset(loopStart===0&&loopEnd===(flags?frames:0),'Partial sound loops are unsupported','AUDIO');
  requireAsset(Number.isFinite(bytes.readFloatLE(28))&&Number.isFinite(bytes.readFloatLE(32)),'Invalid sound loudness metadata','AUDIO');
  for(let p=offset;p<bytes.length;p+=4)requireAsset(Number.isFinite(bytes.readFloatLE(p))&&Math.abs(bytes.readFloatLE(p))<=16,'Invalid sound sample','AUDIO');
  return {channels,sampleRate,frames,seconds:frames/sampleRate};
}
