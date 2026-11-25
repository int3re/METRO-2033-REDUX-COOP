// SHA-256 of a file through Windows' BCrypt: the check of a downloaded launcher against its release.
#pragma once
#include <cstdint>
#include <string>

namespace lz {
bool sha256_file(const std::wstring& path, uint8_t out[32]);  // false: the file cannot be read
std::string to_hex(const uint8_t* data, size_t len);
}  // namespace lz
