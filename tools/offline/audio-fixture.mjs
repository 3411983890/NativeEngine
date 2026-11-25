import fs from 'node:fs/promises';
export async function writeAudioFixture(file,{channels=1,seconds=.25}={}) {
  const frames=Math.round(48000*seconds),bytes=Buffer.alloc(44+frames*channels*2);
  bytes.write('RIFF');bytes.writeUInt32LE(bytes.length-8,4);bytes.write('WAVEfmt ',8);bytes.writeUInt32LE(16,16);bytes.writeUInt16LE(1,20);bytes.writeUInt16LE(channels,22);bytes.writeUInt32LE(48000,24);bytes.writeUInt32LE(48000*channels*2,28);bytes.writeUInt16LE(channels*2,32);bytes.writeUInt16LE(16,34);bytes.write('data',36);bytes.writeUInt32LE(bytes.length-44,40);
  for(let i=0;i<frames;i++)for(let c=0;c<channels;c++)bytes.writeInt16LE(Math.round(Math.sin(2*Math.PI*(440+c*110)*i/48000)*6000),44+(i*channels+c)*2);
  await fs.writeFile(file,bytes);return file;
}
