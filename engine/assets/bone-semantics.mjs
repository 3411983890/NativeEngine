// Shared bone-name semantics for retargeting and rig detection. Names are hints:
// callers must still validate hierarchy, sides and lengths before using them.
export const identifier = name => String(name).split(/[:|/]/).at(-1).toLowerCase()
  .replace(/^mixamorig/, '').replace(/[^a-z0-9]/g, '').replace(/jnt$/, '');

const aliases = new Map();
for (const group of [
  ['root', 'armature', 'skeleton'], ['hips', 'hip', 'pelvis'], ['spine', 'spine0', 'spine01'], ['chest', 'spine2', 'spine02'],
  ['leftarm', 'leftupperarm', 'upperarml', 'lupperarm', 'larm'], ['rightarm', 'rightupperarm', 'upperarmr', 'rupperarm', 'rarm'],
  ['leftforearm', 'leftlowerarm', 'lowerarml', 'lforearm'], ['rightforearm', 'rightlowerarm', 'lowerarmr', 'rforearm'],
  ['lefthand', 'handl', 'lhand'], ['righthand', 'handr', 'rhand'],
  ['leftthigh', 'leftupleg', 'leftupperleg', 'thighl', 'lthigh'], ['rightthigh', 'rightupleg', 'rightupperleg', 'thighr', 'rthigh'],
  ['leftcalf', 'leftleg', 'leftlowerleg', 'calfl', 'lcalf'], ['rightcalf', 'rightleg', 'rightlowerleg', 'calfr', 'rcalf'],
  ['leftfoot', 'footl', 'lfoot'], ['rightfoot', 'footr', 'rfoot'], ['lefttoe', 'lefttoebase', 'balll'], ['righttoe', 'righttoebase', 'ballr'],
  ['head', 'headbone'], ['headend', 'headtopend'], ['neck'], ['leftshoulder', 'shoulderl'], ['rightshoulder', 'shoulderr'],
]) for (const name of group) aliases.set(name, group[0]);

export const boneKey = name => aliases.get(identifier(name)) ?? identifier(name);

export function sideOf(name) {
  const id = identifier(name);
  if (/^(left|leftside)/.test(id)) return 'left';
  if (/^(right|rightside)/.test(id)) return 'right';
  const tokens = String(name).toLowerCase().split(/[:|/_.\s-]+/);
  if (tokens.includes('left') || tokens.includes('l')) return 'left';
  if (tokens.includes('right') || tokens.includes('r')) return 'right';
  return '';
}
