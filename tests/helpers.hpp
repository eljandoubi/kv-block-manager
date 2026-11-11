#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "kvbm/block_manager.hpp"

namespace kvbm::testing {

using Tokens = std::vector<TokenId>;

// from, from + 1, ... (n tokens)
inline Tokens iota_tokens(TokenId from, std::size_t n) {
  Tokens t(n);
  for (std::size_t i = 0; i < n; ++i) t[i] = from + static_cast<TokenId>(i);
  return t;
}

inline Tokens join(std::initializer_list<Tokens> parts) {
  Tokens out;
  for (const Tokens& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// The tokens of a sequence, read back from the blocks it holds.
inline Tokens tokens_of(const BlockManager& m, SeqId id) {
  Tokens out;
  for (const BlockId b : m.block_table(id)) {
    const auto t = m.block_tokens(b);
    out.insert(out.end(), t.begin(), t.end());
  }
  return out;
}

// Everything observable about the manager, to compare before and after an operation that must
// not have changed anything.
struct Snapshot {
  Stats stats;
  std::vector<std::vector<BlockId>> tables;
  std::vector<std::size_t> refs;

  bool operator==(const Snapshot& o) const {
    return stats.used_blocks == o.stats.used_blocks && stats.cached_free_blocks == o.stats.cached_free_blocks &&
           stats.free_blocks == o.stats.free_blocks && stats.evictions == o.stats.evictions &&
           stats.cow_copies == o.stats.cow_copies && stats.hit_blocks == o.stats.hit_blocks &&
           stats.lookup_blocks == o.stats.lookup_blocks && tables == o.tables && refs == o.refs;
  }
};

inline Snapshot snapshot(const BlockManager& m, const std::vector<SeqId>& ids) {
  Snapshot s;
  s.stats = m.stats();
  for (const SeqId id : ids) {
    const auto t = m.block_table(id);
    s.tables.emplace_back(t.begin(), t.end());
  }
  for (std::size_t b = 0; b < m.config().num_blocks; ++b) s.refs.push_back(m.ref_count(static_cast<BlockId>(b)));
  return s;
}

}  // namespace kvbm::testing

#define EXPECT_VALID(manager) EXPECT_EQ((manager).check_invariants(), "")
