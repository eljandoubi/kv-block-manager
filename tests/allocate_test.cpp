#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

#include "helpers.hpp"

using namespace kvbm;
using namespace kvbm::testing;

namespace {

BlockManager make(std::size_t blocks, std::size_t block_size = 4, bool cache = true) {
  return BlockManager(Config{.num_blocks = blocks, .block_size = block_size, .enable_prefix_cache = cache});
}

TEST(Config, ZeroSizesAreRejected) {
  EXPECT_THROW(BlockManager(Config{.num_blocks = 0}), std::invalid_argument);
  EXPECT_THROW(BlockManager(Config{.num_blocks = 4, .block_size = 0}), std::invalid_argument);
  EXPECT_THROW(BlockManager(Config{.num_blocks = kNoBlock}), std::invalid_argument);
}

TEST(Config, TheManagerReportsItsConfiguration) {
  const auto m = make(7, 3, false);
  EXPECT_EQ(m.config().num_blocks, 7u);
  EXPECT_EQ(m.config().block_size, 3u);
  EXPECT_FALSE(m.config().enable_prefix_cache);
  EXPECT_EQ(m.num_available_blocks(), 7u);
}

TEST(Status, HasAReadableName) {
  EXPECT_STREQ(to_string(Status::Ok), "ok");
  EXPECT_STREQ(to_string(Status::OutOfBlocks), "out of blocks");
  EXPECT_STREQ(to_string(Status::UnknownSequence), "unknown sequence");
  EXPECT_STREQ(to_string(Status::DuplicateSequence), "duplicate sequence");
}

TEST(Allocate, AShortPromptTakesOnePartialBlock) {
  auto m = make(8);
  const auto r = m.allocate(1, iota_tokens(0, 3));
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.cached_tokens, 0u);
  ASSERT_EQ(m.block_table(1).size(), 1u);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 3));
  EXPECT_EQ(m.num_tokens(1), 3u);
  const auto s = m.stats();
  EXPECT_EQ(s.used_blocks, 1u);
  EXPECT_EQ(s.free_blocks, 7u);
  EXPECT_EQ(s.cached_free_blocks, 0u);
  EXPECT_VALID(m);
}

TEST(Allocate, TheNumberOfBlocksIsTheLengthOverTheBlockSizeRoundedUp) {
  for (std::size_t n = 0; n <= 13; ++n) {
    auto m = make(8);
    ASSERT_EQ(m.allocate(1, iota_tokens(0, n)).status, Status::Ok) << n;
    EXPECT_EQ(m.block_table(1).size(), (n + 3) / 4) << n;
    EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, n));
    EXPECT_VALID(m);
  }
}

TEST(Allocate, AnEmptyPromptMakesAnEmptySequenceThatCanGrow) {
  auto m = make(4);
  EXPECT_EQ(m.allocate(1, {}).status, Status::Ok);
  EXPECT_TRUE(m.block_table(1).empty());
  EXPECT_EQ(m.num_sequences(), 1u);
  EXPECT_EQ(m.append(1, iota_tokens(0, 5)).status, Status::Ok);
  EXPECT_EQ(m.block_table(1).size(), 2u);
  EXPECT_VALID(m);
}

TEST(Allocate, AnIdInUseIsRefusedAndNothingChanges) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  const auto before = snapshot(m, {1});
  EXPECT_EQ(m.allocate(1, iota_tokens(100, 6)).status, Status::DuplicateSequence);
  EXPECT_EQ(snapshot(m, {1}), before);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 6));
}

TEST(Allocate, UnknownIdsAreReported) {
  auto m = make(4);
  EXPECT_EQ(m.free(9), Status::UnknownSequence);
  EXPECT_EQ(m.append(9, iota_tokens(0, 1)).status, Status::UnknownSequence);
  EXPECT_EQ(m.fork(9, 10), Status::UnknownSequence);
  EXPECT_TRUE(m.block_table(9).empty());
  EXPECT_EQ(m.num_tokens(9), 0u);
  EXPECT_TRUE(m.block_tokens(1000).empty());
  EXPECT_EQ(m.ref_count(1000), 0u);
}

TEST(Allocate, FreeGivesTheBlocksBack) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 10));  // two full blocks and a partial one
  EXPECT_EQ(m.num_available_blocks(), 1u);
  EXPECT_EQ(m.free(1), Status::Ok);
  EXPECT_EQ(m.num_available_blocks(), 4u);
  EXPECT_EQ(m.num_sequences(), 0u);
  EXPECT_EQ(m.free(1), Status::UnknownSequence);
  EXPECT_VALID(m);
}

TEST(Allocate, RunningOutOfBlocksIsRefusedAndNothingChanges) {
  auto m = make(3);
  m.allocate(1, iota_tokens(0, 8));  // two blocks
  const auto before = snapshot(m, {1});
  const auto r = m.allocate(2, iota_tokens(100, 8));  // needs two, one is left
  EXPECT_EQ(r.status, Status::OutOfBlocks);
  EXPECT_EQ(r.cached_tokens, 0u);
  EXPECT_EQ(snapshot(m, {1}), before);
  EXPECT_EQ(m.num_sequences(), 1u);
  EXPECT_EQ(m.stats().failed_requests, 1u);
  EXPECT_VALID(m);
}

TEST(Allocate, CanAllocateAgreesWithAllocate) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 12));  // three blocks, one left
  EXPECT_TRUE(m.can_allocate(iota_tokens(100, 4)));
  EXPECT_FALSE(m.can_allocate(iota_tokens(100, 5)));
  EXPECT_TRUE(m.can_allocate({}));
  EXPECT_EQ(m.allocate(2, iota_tokens(100, 5)).status, Status::OutOfBlocks);
  EXPECT_EQ(m.allocate(2, iota_tokens(100, 4)).status, Status::Ok);
}

TEST(Prefix, AnIdenticalPromptReusesEveryBlockButTheOneHoldingTheLastToken) {
  auto m = make(16);
  const auto prompt = iota_tokens(0, 12);  // three full blocks
  m.allocate(1, prompt);
  const auto r = m.allocate(2, prompt);
  EXPECT_EQ(r.cached_tokens, 8u);  // blocks 0 and 1; the last token has to be computed
  const auto a = m.block_table(1);
  const auto b = m.block_table(2);
  EXPECT_EQ(a[0], b[0]);
  EXPECT_EQ(a[1], b[1]);
  EXPECT_NE(a[2], b[2]);
  EXPECT_EQ(m.ref_count(a[0]), 2u);
  EXPECT_EQ(m.ref_count(a[1]), 2u);
  EXPECT_EQ(m.ref_count(a[2]), 1u);
  EXPECT_EQ(m.ref_count(b[2]), 1u);
  EXPECT_EQ(tokens_of(m, 2), prompt);
  EXPECT_EQ(m.stats().used_blocks, 4u);
  EXPECT_VALID(m);
}

TEST(Prefix, APartialTailDoesNotStopTheFullBlocksFromBeingReused) {
  auto m = make(16);
  const auto prompt = iota_tokens(0, 10);  // two full blocks and two tokens
  m.allocate(1, prompt);
  const auto r = m.allocate(2, prompt);
  EXPECT_EQ(r.cached_tokens, 8u);
  EXPECT_EQ(m.stats().hit_blocks, 2u);
  EXPECT_EQ(m.stats().lookup_blocks, 4u);  // two full blocks in each of the two prompts
  EXPECT_DOUBLE_EQ(m.stats().hit_rate(), 0.5);
  EXPECT_NE(m.block_table(1)[2], m.block_table(2)[2]);  // partial tails are never shared
}

TEST(Prefix, ABlockInCommonIsSharedAndWhatFollowsIsNot) {
  auto m = make(16);
  m.allocate(1, join({iota_tokens(0, 4), iota_tokens(100, 5)}));
  const auto r = m.allocate(2, join({iota_tokens(0, 4), iota_tokens(200, 5)}));
  EXPECT_EQ(r.cached_tokens, 4u);
  EXPECT_EQ(m.block_table(1)[0], m.block_table(2)[0]);
  EXPECT_NE(m.block_table(1)[1], m.block_table(2)[1]);
  EXPECT_VALID(m);
}

TEST(Prefix, TheSameTokensAfterADifferentPrefixAreNotShared) {
  // KV values depend on everything before them, so block [50..53] after [0..3] is not the same
  // thing as block [50..53] after [10..13]
  auto m = make(16);
  const auto b = iota_tokens(50, 4);
  m.allocate(1, join({iota_tokens(0, 4), b, {99}}));
  const auto r = m.allocate(2, join({iota_tokens(10, 4), b, {99}}));
  EXPECT_EQ(r.cached_tokens, 0u);
  EXPECT_NE(m.block_table(1)[1], m.block_table(2)[1]);
  EXPECT_EQ(m.ref_count(m.block_table(1)[1]), 1u);
}

TEST(Prefix, ReleasedBlocksStayCachedAndAreFoundAgain) {
  auto m = make(16);
  const auto prompt = iota_tokens(0, 9);  // two full blocks and one token
  m.allocate(1, prompt);
  m.free(1);
  auto s = m.stats();
  EXPECT_EQ(s.cached_free_blocks, 2u);
  EXPECT_EQ(s.free_blocks, 14u);
  EXPECT_EQ(s.used_blocks, 0u);
  const auto r = m.allocate(2, prompt);
  EXPECT_EQ(r.cached_tokens, 8u);
  s = m.stats();
  EXPECT_EQ(s.cached_free_blocks, 0u);
  EXPECT_EQ(s.used_blocks, 3u);
  EXPECT_VALID(m);
}

TEST(Prefix, WithTheCacheOffNothingIsSharedOrKept) {
  auto m = make(16, 4, false);
  const auto prompt = iota_tokens(0, 9);
  m.allocate(1, prompt);
  EXPECT_EQ(m.allocate(2, prompt).cached_tokens, 0u);
  EXPECT_EQ(m.stats().used_blocks, 6u);
  m.free(1);
  m.free(2);
  EXPECT_EQ(m.stats().cached_free_blocks, 0u);
  EXPECT_EQ(m.stats().free_blocks, 16u);
  EXPECT_EQ(m.stats().hit_blocks, 0u);
  EXPECT_VALID(m);
}

TEST(Prefix, GeneratedTokensJoinTheCacheWhenTheirBlockFills) {
  auto m = make(16);
  m.allocate(1, iota_tokens(0, 6));  // block [0..3] and a partial one [4,5]
  m.append(1, iota_tokens(6, 2));    // the partial one is now [4..7]: full
  m.free(1);
  // a later prompt: the old prompt, what was generated, and one more token
  const auto r = m.allocate(2, iota_tokens(0, 9));
  EXPECT_EQ(r.cached_tokens, 8u);
  EXPECT_EQ(m.stats().hit_blocks, 2u);
}

TEST(Prefix, ACachedBlockThatIsReusedCannotAlsoCountAsRoomForTheRest) {
  auto m = make(3);
  m.allocate(1, iota_tokens(0, 5));
  m.free(1);  // block [0..3] is cached, two blocks are empty: three are available
  const auto before = snapshot(m, {});
  // four blocks, one of them reused: three new ones are needed but only two are empty
  EXPECT_EQ(m.allocate(2, iota_tokens(0, 13)).status, Status::OutOfBlocks);
  EXPECT_EQ(snapshot(m, {}), before);
  EXPECT_EQ(m.stats().cached_free_blocks, 1u);
  // three blocks, one reused: two new ones are needed and there are two
  EXPECT_EQ(m.allocate(3, iota_tokens(0, 9)).status, Status::Ok);
  EXPECT_EQ(m.stats().hit_blocks, 1u);
  EXPECT_VALID(m);
}

TEST(Prefix, ABlockSizeOfOneWorks) {
  auto m = make(16, 1);
  m.allocate(1, iota_tokens(0, 5));
  const auto r = m.allocate(2, iota_tokens(0, 5));
  EXPECT_EQ(r.cached_tokens, 4u);
  EXPECT_EQ(m.stats().used_blocks, 6u);
  EXPECT_VALID(m);
}

}  // namespace
