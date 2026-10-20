#pragma once

#include <cstddef>
#include <string>

namespace nativegpu {

// Writes an 8-bit RGBA image as a PNG using stored (uncompressed) deflate blocks.
// The encoder is intentionally dependency-free: screenshots are evidence files that
// must be produced by the shipped binary on any machine, including CI, without pulling
// in an image library. File size is irrelevant for a 1-2 MB frame capture.
bool writePng(const std::string& path, unsigned width, unsigned height,
              const unsigned char* rgba, std::size_t rowPitch, std::string& error);

} // namespace nativegpu
