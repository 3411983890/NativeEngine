#pragma once
namespace engine {
// Two affine rows and a relative index in the bounded 109-entry sampler heap.
struct TextureMapping {
    float row0[3]{1,0,0}; unsigned sampler=0;
    float row1[3]{0,1,0}; unsigned padding=0;
};
static_assert(sizeof(TextureMapping)==32,"Texture mapping packing changed");
}
