#pragma once

#include <string>

namespace engine::assets {

// Windows CNG SHA-256. Every cooked asset authenticates its payload bytes with this,
// so the implementation lives in one place instead of being copied per asset type.
std::string sha256(const std::string& bytes);

} // namespace engine::assets
