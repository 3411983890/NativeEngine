#pragma once
#include <filesystem>
namespace engine::assets {
// Content-addressed packages legitimately nest two SHA-256 directory/file
// names. Use wide extended paths at the I/O boundary, without changing authored
// relative references or bypassing canonical containment checks.
inline std::filesystem::path nativeFilePath(const std::filesystem::path& input) {
#ifdef _WIN32
    auto supplied=input;supplied.make_preferred();const auto& existing=supplied.native();
    if(existing.rfind(L"\\\\?\\",0)==0||existing.rfind(L"\\\\.\\",0)==0)return supplied;
    auto absolute=std::filesystem::absolute(input).lexically_normal();absolute.make_preferred();
    const auto& value=absolute.native();
    if(value.rfind(L"\\\\",0)==0)return std::filesystem::path(L"\\\\?\\UNC\\"+value.substr(2));
    return std::filesystem::path(L"\\\\?\\"+value);
#else
    return input;
#endif
}
inline std::filesystem::path canonicalFilePath(const std::filesystem::path& input) {
    return nativeFilePath(std::filesystem::canonical(nativeFilePath(input)));
}
inline std::filesystem::path weaklyCanonicalFilePath(const std::filesystem::path& input) {
    return nativeFilePath(std::filesystem::weakly_canonical(nativeFilePath(input)));
}
}
