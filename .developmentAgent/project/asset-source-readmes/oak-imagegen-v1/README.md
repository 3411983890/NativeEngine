# Oak texture prototypes

Generated: 2026-10-25 using built-in image_gen (not CLI).

These are authoring prototypes, not validated production PBR maps. No engine material or texture manifest is changed.

- oak_bark_basecolor_prototype.png: visually generated bark color source. Residual relief shading and repeat-edge continuity require validation/correction before use as final albedo.
- oak_leaves_basecolor_alpha_prototype.png: twelve-leaf color atlas requested with transparent background. Alpha edges, padding, holes and mip coverage require validation; some leaves are close to atlas borders.
- Update 2026-10-25: Normal, roughness, AO, thickness, height and packed ORM authoring maps are now supplied in `complete/`; see `complete/README.md`. Mip chains and cooked runtime textures are not included. AI-inferred data need material-preview calibration and are not physical measurements.

## Bark prompt

Use case: photorealistic-natural. Asset type: game PBR texture source, oak bark BASE COLOR ONLY. Generate a square, edge-to-edge orthographic flattened scan of mature oak trunk bark, vertically oriented interlocking gray-brown ridges and fissures, restrained natural warm gray and brown variations, fine pores and aged flakes. Even diffuse cross-polarized illumination, remove directional lighting, cast shadows, specular highlights, ambient occlusion and perspective. No tree silhouette, no forest, no frame, no labels, no text, no watermarks, no collage, no multiple maps. Entire canvas is bark. Make opposing edges visually seamless for repeat tiling, avoid conspicuous unique features and gradients. High resolution crisp realistic surface detail, not oversharpened. This is an albedo authoring prototype, not a lit material preview.

## Leaves prompt

Use case: photorealistic-natural. Asset type: game oak LEAF BASE COLOR AND ALPHA texture atlas. Generate a square atlas on a truly transparent background, twelve isolated pedunculate oak leaves with rounded lobes, arranged in a tidy 3 columns by 4 rows layout, one leaf per cell, never overlapping. All leaves and short petioles fully visible with generous transparent gutters and outer padding. Orthographic flat top-down, leaves flattened in the same plane, tips pointing up and petioles down, realistic green oak lamina with fine veins. Nine healthy mature leaf variations, two lighter young leaves, one subtly yellowing leaf with tiny insect holes. No branches, no ground, no shadows, no specular reflections, no depth of field, no perspective, no labels, no grid lines, no checkerboard painted in image. Even cross-polarized diffuse illumination for albedo authoring, no dramatic baked lighting, restrained natural color variations, sharp natural edges and detailed veins. Preserve genuine alpha around leaf silhouettes and insect holes. All twelve leaves visually distinct but same species.
