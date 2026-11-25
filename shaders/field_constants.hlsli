// Shared 28-DWORD immutable sparse snapshot query ABI, included inside b0/b1.
int3 regionMin; uint pageCount;
int3 regionMax; uint queryCount;
uint4 channels0; uint4 channels1;
uint2 snapshotTick; uint2 snapshotVersion;
uint2 requestedTick; uint2 maxAge;
uint consistency; uint channelCount; uint haloCells; uint padding;
