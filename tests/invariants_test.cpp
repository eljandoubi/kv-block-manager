// The invariant checker is what the other tests lean on, so it gets tested too: each test damages
// one thing inside a healthy manager and expects the checker to say what is wrong.

#include <gtest/gtest.h>

#include <string>

#include "helpers.hpp"

namespace kvbm {

// Declared a friend of BlockManager: reaches into its private state.
struct BlockManagerTestAccess {
  static auto& blocks(BlockManager& m) { return m.blocks_; }
  static auto& free_list(BlockManager& m) { return m.free_; }
  static auto& by_hash(BlockManager& m) { return m.by_hash_; }
  static auto& seqs(BlockManager& m) { return m.seqs_; }
  static auto& lru_size(BlockManager& m) { return m.lru_size_; }
  static auto& lru_tail(BlockManager& m) { return m.lru_tail_; }
};

}  // namespace kvbm

using namespace kvbm;
using namespace kvbm::testing;
using Access = kvbm::BlockManagerTestAccess;

namespace {

// Two sequences sharing a block, a cached block and an empty one: a bit of everything.
BlockManager healthy() {
  BlockManager m(Config{.num_blocks = 8, .block_size = 4});
  m.allocate(1, iota_tokens(0, 10));   // blocks A B and a partial C
  m.allocate(2, iota_tokens(0, 6));    // shares A, owns a partial
  m.allocate(3, iota_tokens(100, 5));  // a full block and a partial one
  m.free(3);                           // the full block is cached, the partial one is empty
  EXPECT_EQ(m.check_invariants(), "");
  return m;
}

::testing::AssertionResult complains(const BlockManager& m, const std::string& about) {
  const std::string said = m.check_invariants();
  if (said.empty()) return ::testing::AssertionFailure() << "the checker saw nothing wrong";
  if (said.find(about) == std::string::npos)
    return ::testing::AssertionFailure() << "it said '" << said << "', not about '" << about << "'";
  return ::testing::AssertionSuccess();
}

TEST(Invariants, AHealthyManagerPasses) {
  auto m = healthy();
  EXPECT_EQ(m.check_invariants(), "");
}

TEST(Invariants, ACountThatDisagreesWithTheSequencesIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[0]].ref += 1;
  EXPECT_TRUE(complains(m, "references"));
}

TEST(Invariants, ABlockOnTheFreeListTwiceIsFound) {
  auto m = healthy();
  Access::free_list(m).push_back(Access::free_list(m).back());
  EXPECT_TRUE(complains(m, "twice"));
}

TEST(Invariants, ABlockInUseOnTheFreeListIsFound) {
  auto m = healthy();
  Access::free_list(m).push_back(m.block_table(1)[0]);
  EXPECT_TRUE(complains(m, "not empty"));
}

TEST(Invariants, AnUnknownBlockOnTheFreeListIsFound) {
  auto m = healthy();
  Access::free_list(m).push_back(999);
  EXPECT_TRUE(complains(m, "unknown block"));
}

TEST(Invariants, ALostBlockIsFound) {
  auto m = healthy();
  Access::free_list(m).pop_back();
  EXPECT_TRUE(complains(m, "lost"));
}

TEST(Invariants, AWrongLruSizeIsFound) {
  auto m = healthy();
  Access::lru_size(m) += 1;
  EXPECT_TRUE(complains(m, "LRU size"));
}

TEST(Invariants, AWrongLruTailIsFound) {
  auto m = healthy();
  Access::lru_tail(m) = kNoBlock;
  EXPECT_TRUE(complains(m, "LRU tail"));
}

TEST(Invariants, ABrokenLruBackLinkIsFound) {
  auto m = healthy();
  // two cached blocks, so the second one has a back link to break
  m.free(1);
  m.free(2);
  ASSERT_GE(m.stats().cached_free_blocks, 2u);
  for (auto& b : Access::blocks(m)) {
    if (b.in_lru && b.lru_prev != kNoBlock) b.lru_prev = kNoBlock;
  }
  EXPECT_TRUE(complains(m, "back link"));
}

TEST(Invariants, AStaleHashMapEntryIsFound) {
  auto m = healthy();
  Access::by_hash(m)[12345] = m.block_table(1)[0];
  EXPECT_TRUE(complains(m, "stale"));
}

TEST(Invariants, AHashMapEntryForAnUnknownBlockIsFound) {
  auto m = healthy();
  Access::by_hash(m)[12345] = 999;
  EXPECT_TRUE(complains(m, "unknown block"));
}

TEST(Invariants, AFreeBlockInTheHashMapIsFound) {
  auto m = healthy();
  const BlockId empty = Access::free_list(m).back();
  Access::by_hash(m)[777] = empty;
  EXPECT_TRUE(complains(m, "stale"));
}

TEST(Invariants, ABlockThatClaimsAMapEntryItDoesNotHaveIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[2]].in_map = true;  // the partial block of sequence 1
  EXPECT_TRUE(complains(m, "claim one"));
}

TEST(Invariants, ACachedBlockMissingFromTheHashMapIsFound) {
  auto m = healthy();
  Access::by_hash(m).clear();
  EXPECT_TRUE(complains(m, "holder of its hash"));
}

TEST(Invariants, ACachedBlockWhoseHashPointsAtAnotherBlockIsFound) {
  auto m = healthy();
  for (const auto& b : Access::blocks(m)) {
    if (b.in_lru) Access::by_hash(m)[b.hash] = m.block_table(1)[0];
  }
  EXPECT_TRUE(complains(m, "holder of its hash"));
}

TEST(Invariants, ATableOfTheWrongLengthIsFound) {
  auto m = healthy();
  Access::seqs(m).at(1).table.pop_back();
  EXPECT_TRUE(complains(m, "wrong number of blocks"));
}

TEST(Invariants, ATableWithAnUnknownBlockIsFound) {
  auto m = healthy();
  Access::seqs(m).at(1).table[0] = 999;
  EXPECT_TRUE(complains(m, "unknown block"));
}

TEST(Invariants, ABlockWithTheWrongNumberOfTokensIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[2]].tokens.push_back(1);
  EXPECT_TRUE(complains(m, "tokens, not"));
}

TEST(Invariants, AFullBlockWhoseHashDiffersFromItsMapEntryIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[1]].hash ^= 1;
  EXPECT_TRUE(complains(m, "stale"));
}

TEST(Invariants, AFullBlockThatIsNotHashedIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[1]].hashed = false;
  EXPECT_TRUE(complains(m, "stale"));
}

TEST(Invariants, AFullBlockWithAWrongParentHashIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[1]].parent_hash ^= 1;
  EXPECT_TRUE(complains(m, "wrong chain hash"));
}

TEST(Invariants, APartialBlockThatIsHashedIsFound) {
  auto m = healthy();
  Access::blocks(m)[m.block_table(1)[2]].hashed = true;
  EXPECT_TRUE(complains(m, "is hashed"));
}

TEST(Invariants, AWrongLastHashOfASequenceIsFound) {
  auto m = healthy();
  Access::seqs(m).at(1).last_full_hash ^= 1;
  EXPECT_TRUE(complains(m, "last hash"));
}

TEST(Invariants, AnLruBlockThatIsAlsoOnTheFreeListIsFound) {
  auto m = healthy();
  for (auto& b : Access::blocks(m)) {
    if (b.in_lru) b.lru_next = Access::free_list(m).back();
  }
  EXPECT_TRUE(complains(m, "twice or on the free list"));
}

TEST(Invariants, AnLruBlockThatIsNotCachedIsFound) {
  auto m = healthy();
  for (auto& b : Access::blocks(m)) {
    if (b.in_lru) b.in_map = false;
  }
  EXPECT_TRUE(complains(m, "not cached and unused"));
}

TEST(Invariants, WithTheCacheOffTheChainIsNotChecked) {
  BlockManager m(Config{.num_blocks = 8, .block_size = 4, .enable_prefix_cache = false});
  m.allocate(1, iota_tokens(0, 9));
  EXPECT_EQ(m.check_invariants(), "");
}

}  // namespace
