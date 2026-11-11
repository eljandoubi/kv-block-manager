#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace kvbm;
using namespace kvbm::testing;

namespace {

BlockManager make(std::size_t blocks, std::size_t block_size = 4, bool cache = true) {
  return BlockManager(Config{.num_blocks = blocks, .block_size = block_size, .enable_prefix_cache = cache});
}

TEST(Eviction, ACachedBlockIsOnlyEvictedWhenNothingEmptyIsLeft) {
  auto m = make(3);
  m.allocate(1, iota_tokens(0, 5));    // a full block and one token
  m.free(1);                           // the full block stays cached, two blocks are empty
  m.allocate(2, iota_tokens(100, 5));  // takes the two empty ones
  EXPECT_EQ(m.stats().evictions, 0u);
  EXPECT_EQ(m.stats().cached_free_blocks, 1u);
  EXPECT_EQ(m.allocate(3, iota_tokens(200, 1)).status, Status::Ok);  // now it has to evict
  EXPECT_EQ(m.stats().evictions, 1u);
  EXPECT_EQ(m.stats().cached_free_blocks, 0u);
  m.free(2);
  m.free(3);
  EXPECT_EQ(m.allocate(4, iota_tokens(0, 5)).cached_tokens, 0u);  // the evicted block is gone
  EXPECT_VALID(m);
}

TEST(Eviction, TheOldestCachedBlockGoesFirst) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 5));
  m.free(1);  // A is cached
  m.allocate(2, iota_tokens(100, 5));
  m.free(2);                           // B is cached, newer than A
  m.allocate(3, iota_tokens(200, 9));  // three blocks: the two empty ones, then A
  EXPECT_EQ(m.stats().evictions, 1u);
  m.free(3);
  EXPECT_EQ(m.allocate(4, iota_tokens(100, 5)).cached_tokens, 4u);  // B survived
  m.free(4);
  EXPECT_EQ(m.allocate(5, iota_tokens(0, 5)).cached_tokens, 0u);  // A did not
  EXPECT_VALID(m);
}

TEST(Eviction, ReusingABlockMakesItTheNewestSoItOutlivesOlderOnes) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 5));
  m.free(1);  // A cached
  m.allocate(2, iota_tokens(100, 5));
  m.free(2);  // B cached, newer
  m.allocate(3, iota_tokens(0, 5));
  m.free(3);  // A used again: now A is the newer one
  m.allocate(4, iota_tokens(200, 9));
  EXPECT_EQ(m.stats().evictions, 1u);  // B was the oldest
  m.free(4);
  EXPECT_EQ(m.allocate(5, iota_tokens(0, 5)).cached_tokens, 4u);
  m.free(5);
  EXPECT_EQ(m.allocate(6, iota_tokens(100, 5)).cached_tokens, 0u);
  EXPECT_VALID(m);
}

TEST(Eviction, FreeingTheTailFirstKeepsTheHeadOfAPromptCachedLongest) {
  auto m = make(6);
  const auto prompt = iota_tokens(0, 13);  // three full blocks and one token
  m.allocate(1, prompt);
  m.free(1);                            // three cached blocks, three empty
  m.allocate(2, iota_tokens(500, 16));  // four blocks: three empty, one eviction
  EXPECT_EQ(m.stats().evictions, 1u);
  m.free(2);
  // the eviction took the third block of the old prompt, so its first two are still there
  EXPECT_EQ(m.allocate(3, prompt).cached_tokens, 8u);
  EXPECT_VALID(m);
}

TEST(Eviction, BlocksInUseAreNeverTakenAway) {
  auto m = make(3);
  m.allocate(1, iota_tokens(0, 4));
  m.allocate(2, iota_tokens(10, 4));
  m.allocate(3, iota_tokens(20, 4));
  EXPECT_EQ(m.allocate(4, iota_tokens(30, 1)).status, Status::OutOfBlocks);
  EXPECT_EQ(m.stats().evictions, 0u);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 4));
  EXPECT_EQ(tokens_of(m, 2), iota_tokens(10, 4));
  EXPECT_EQ(tokens_of(m, 3), iota_tokens(20, 4));
  EXPECT_VALID(m);
}

TEST(Eviction, AppendingEvictsCachedBlocksToo) {
  auto m = make(3);
  m.allocate(1, iota_tokens(0, 4));
  m.free(1);                                                       // one cached block, two empty
  m.allocate(2, iota_tokens(100, 4));                              // one empty block used
  EXPECT_EQ(m.append(2, iota_tokens(104, 8)).status, Status::Ok);  // two more: one empty, one eviction
  EXPECT_EQ(m.stats().evictions, 1u);
  EXPECT_EQ(m.stats().free_blocks, 0u);
  EXPECT_VALID(m);
}

TEST(Eviction, CopyOnWriteCanEvictToo) {
  auto m = make(3);
  m.allocate(9, iota_tokens(50, 4));
  m.free(9);                         // one cached block
  m.allocate(1, iota_tokens(0, 6));  // full + partial: the two empty blocks
  m.fork(1, 2);
  EXPECT_EQ(m.append(2, Tokens{7}).copies.size(), 1u);  // the copy evicts the cached block
  EXPECT_EQ(m.stats().evictions, 1u);
  EXPECT_VALID(m);
}

TEST(Eviction, TwoSequencesThatBuildTheSameBlockSeparatelyKeepOneCopyCached) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 2));
  m.allocate(2, iota_tokens(0, 3));
  m.append(1, iota_tokens(2, 2));  // [0..3] is full and registered
  m.append(2, iota_tokens(3, 1));  // [0..3] again, built apart: the hash is taken
  EXPECT_VALID(m);
  m.free(1);
  m.free(2);
  const auto s = m.stats();
  EXPECT_EQ(s.cached_free_blocks, 1u);
  EXPECT_EQ(s.free_blocks, 7u);
  EXPECT_EQ(m.allocate(3, iota_tokens(0, 5)).cached_tokens, 4u);
  EXPECT_VALID(m);
}

TEST(Eviction, WithTheCacheOffFreedBlocksGoStraightBackToTheEmptyList) {
  auto m = make(3, 4, false);
  m.allocate(1, iota_tokens(0, 12));
  m.free(1);
  EXPECT_EQ(m.stats().free_blocks, 3u);
  EXPECT_EQ(m.stats().cached_free_blocks, 0u);
  m.allocate(2, iota_tokens(0, 12));
  EXPECT_EQ(m.stats().evictions, 0u);
  EXPECT_EQ(m.stats().hit_blocks, 0u);
}

TEST(Eviction, AWholePoolOfCachedBlocksCanAllBeReclaimed) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 16));
  m.free(1);  // four cached blocks, none empty
  EXPECT_EQ(m.stats().cached_free_blocks, 4u);
  EXPECT_EQ(m.num_available_blocks(), 4u);
  EXPECT_EQ(m.allocate(2, iota_tokens(500, 16)).status, Status::Ok);
  EXPECT_EQ(m.stats().evictions, 4u);
  EXPECT_EQ(m.stats().cached_free_blocks, 0u);
  EXPECT_VALID(m);
}

TEST(Stats, ZeroLookupsGiveAHitRateOfZero) {
  Stats s;
  EXPECT_DOUBLE_EQ(s.hit_rate(), 0.0);
  s.lookup_blocks = 8;
  s.hit_blocks = 2;
  EXPECT_DOUBLE_EQ(s.hit_rate(), 0.25);
}

}  // namespace
