#include "sha256.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <limits>
#include <stdexcept>

namespace engine::assets {

std::string sha256(const std::string& bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) throw std::runtime_error("SHA input too large");
    struct Provider {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~Provider() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } provider;
    unsigned char output[32]{};
    if (BCryptOpenAlgorithmProvider(&provider.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(provider.algorithm, &provider.hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(provider.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                       static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(provider.hash, output, sizeof(output), 0) < 0)
        throw std::runtime_error("Windows SHA-256 provider failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : output) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

} // namespace engine::assets
