#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace kvbm {

using TokenId = std::int32_t;

// The chain hash of the empty prefix.
inline constexpr std::uint64_t kRootHash = 0x9E3779B97F4A7C15ULL;

// The splitmix64 finalizer: every input bit changes about half of the output bits.
constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Hash of one block of tokens that follows a prefix whose hash is `parent`. Two blocks with the same
// tokens but different prefixes get different hashes, because KV values depend on everything before
// them, not just on the block's own tokens.
constexpr std::uint64_t hash_block(std::uint64_t parent, std::span<const TokenId> tokens) noexcept {
  std::uint64_t h = parent;
  for (const TokenId t : tokens) {
    h = mix64(h ^ static_cast<std::uint32_t>(t));
  }
  return mix64(h ^ static_cast<std::uint64_t>(tokens.size()));
}

}  // namespace kvbm
