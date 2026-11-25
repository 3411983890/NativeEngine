// Float32 material values must remain finite in the native renderer.
export const MAX_IOR=3.4028234663852886e38;
export const validIor=value=>Number.isFinite(value)&&(value===0||value>=1&&value<=MAX_IOR);
export function validateIor(value){if(!validIor(value))throw new Error('ior must be 0 or a finite number in [1, Float32 maximum]');return value;}
export function dielectricF0(ior=1.5){validateIor(ior);return ior===0?1:((ior-1)/(ior+1))**2;}
