// The wind model, in one place, so a test can reason about it without a GPU.
//
// The authority is `shaders/vegetation.hlsl` VSMain (the three lines that compute `wave`, `gust`
// and the displacement). This module exists because the interesting property — "a fork stays
// welded while the canopy moves" — is a property of the *vertex data* (equal wind weights and
// equal rest positions at the junction), and it can be exercised over hundreds of frames on the
// CPU. It is not a second implementation used for rendering: nothing in the cook or the renderer
// calls it.
//
// Keeping the formula here rather than in the test matters: if the shader's model changes, both
// the renderer and this mirror have to be updated deliberately, and the comment names the lines.

// Mirrors VSMain:
//   wave = sin(time * frequency + phase * 2pi + local.y * 0.35)
//   gust = 0.6 + 0.4 * sin(time * frequency * 0.31 + phase * 3.1)
//   world += windDirection * (wave * gust * windWeight * amplitude * scale)
export function windOffset(weight, { phase = 0, seconds = 0, amplitude = 0.1, frequency = 0.6, direction = [1, 0, 0], localY = 0, scale = 1 } = {}) {
  const wave = Math.sin(seconds * frequency + phase * 6.2831 + localY * 0.35);
  const gust = 0.6 + 0.4 * Math.sin(seconds * frequency * 0.31 + phase * 3.1);
  const magnitude = wave * gust * weight * amplitude * scale;
  return [direction[0] * magnitude, direction[1] * magnitude, direction[2] * magnitude];
}

// Applies the model to a point. Used by the sequence test to move a junction's two endpoints.
export function displace(point, weight, options) {
  const offset = windOffset(weight, { ...options, localY: point[1] });
  return [point[0] + offset[0], point[1] + offset[1], point[2] + offset[2]];
}
