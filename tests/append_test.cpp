#include <gtest/gtest.h>

#include <algorithm>

#include "helpers.hpp"

using namespace kvbm;
using namespace kvbm::testing;

namespace {

BlockManager make(std::size_t blocks, std::size_t block_size = 4, bool cache = true) {
  return BlockManager(Config{.num_blocks = blocks, .block_size = block_size, .enable_prefix_cache = cache});
}

TEST(Append, FillsTheLastBlockBeforeTakingAnotherOne) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 2));
  EXPECT_EQ(m.append(1, iota_tokens(2, 2)).status, Status::Ok);
  EXPECT_EQ(m.block_table(1).size(), 1u);
  EXPECT_EQ(m.append(1, iota_tokens(4, 1)).status, Status::Ok);
  EXPECT_EQ(m.block_table(1).size(), 2u);
  EXPECT_EQ(m.num_tokens(1), 5u);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 5));
  EXPECT_VALID(m);
}

TEST(Append, ManyTokensAtOnceTakeAsManyBlocksAsNeeded) {
  auto m = make(8);
  m.allocate(1, {});
  EXPECT_EQ(m.append(1, iota_tokens(0, 10)).status, Status::Ok);
  EXPECT_EQ(m.block_table(1).size(), 3u);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 10));
  EXPECT_VALID(m);
}

TEST(Append, TokensOneByOneGiveTheSameBlocksAsAllAtOnce) {
  auto one_by_one = make(8);
  auto at_once = make(8);
  one_by_one.allocate(1, iota_tokens(0, 3));
  at_once.allocate(1, iota_tokens(0, 3));
  for (TokenId t = 3; t < 14; ++t) one_by_one.append(1, Tokens{t});
  at_once.append(1, iota_tokens(3, 11));
  EXPECT_EQ(tokens_of(one_by_one, 1), tokens_of(at_once, 1));
  EXPECT_EQ(one_by_one.stats().used_blocks, at_once.stats().used_blocks);
  EXPECT_VALID(one_by_one);
  EXPECT_VALID(at_once);
  // and both seal the same blocks: a new prompt finds them either way
  one_by_one.free(1);
  at_once.free(1);
  EXPECT_EQ(one_by_one.allocate(2, iota_tokens(0, 14)).cached_tokens, 12u);
  EXPECT_EQ(at_once.allocate(2, iota_tokens(0, 14)).cached_tokens, 12u);
}

TEST(Append, NothingToAddIsFine) {
  auto m = make(4);
  m.allocate(1, iota_tokens(0, 3));
  const auto before = snapshot(m, {1});
  EXPECT_EQ(m.append(1, {}).status, Status::Ok);
  EXPECT_EQ(snapshot(m, {1}), before);
}

TEST(Append, NotEnoughBlocksChangesNothing) {
  auto m = make(2);
  m.allocate(1, iota_tokens(0, 4));  // one full block, one block left
  const auto before = snapshot(m, {1});
  EXPECT_EQ(m.append(1, iota_tokens(4, 5)).status, Status::OutOfBlocks);  // needs two
  EXPECT_EQ(snapshot(m, {1}), before);
  EXPECT_EQ(m.stats().failed_requests, 1u);
  EXPECT_EQ(m.append(1, iota_tokens(4, 4)).status, Status::Ok);  // needs one
  EXPECT_VALID(m);
}

TEST(Fork, TheChildSharesEveryBlock) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));  // a full block and a partial one
  EXPECT_EQ(m.fork(1, 2), Status::Ok);
  const auto a = m.block_table(1);
  const auto b = m.block_table(2);
  ASSERT_EQ(a.size(), 2u);
  EXPECT_TRUE(std::equal(a.begin(), a.end(), b.begin(), b.end()));
  EXPECT_EQ(m.ref_count(a[0]), 2u);
  EXPECT_EQ(m.ref_count(a[1]), 2u);
  EXPECT_EQ(m.num_tokens(2), 6u);
  EXPECT_EQ(m.stats().used_blocks, 2u);
  EXPECT_VALID(m);
}

TEST(Fork, WritingToASharedPartialBlockCopiesItFirst) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  m.fork(1, 2);
  const BlockId shared = m.block_table(1)[1];
  const auto r = m.append(2, Tokens{99});
  ASSERT_EQ(r.status, Status::Ok);
  ASSERT_EQ(r.copies.size(), 1u);
  EXPECT_EQ(r.copies[0].src, shared);
  EXPECT_EQ(r.copies[0].dst, m.block_table(2)[1]);
  EXPECT_NE(r.copies[0].dst, shared);
  EXPECT_EQ(tokens_of(m, 2), join({iota_tokens(0, 6), {99}}));
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 6));    // the parent never sees it
  EXPECT_EQ(m.ref_count(shared), 1u);               // only the parent reads it now
  EXPECT_EQ(m.ref_count(m.block_table(1)[0]), 2u);  // the full block is still shared
  EXPECT_EQ(m.stats().cow_copies, 1u);
  EXPECT_VALID(m);
}

TEST(Fork, TheParentWritesInPlaceOnceTheChildHasItsOwnCopy) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  m.fork(1, 2);
  m.append(2, Tokens{99});
  const BlockId last = m.block_table(1)[1];
  const auto r = m.append(1, Tokens{77});
  EXPECT_TRUE(r.copies.empty());
  EXPECT_EQ(m.block_table(1)[1], last);
  EXPECT_EQ(tokens_of(m, 1), join({iota_tokens(0, 6), {77}}));
  EXPECT_EQ(tokens_of(m, 2), join({iota_tokens(0, 6), {99}}));
  EXPECT_VALID(m);
}

TEST(Fork, NothingIsCopiedWhenTheLastBlockIsFull) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 8));
  m.fork(1, 2);
  const auto r = m.append(2, Tokens{9});
  EXPECT_TRUE(r.copies.empty());
  EXPECT_EQ(m.block_table(2).size(), 3u);
  EXPECT_EQ(m.ref_count(m.block_table(1)[0]), 2u);
  EXPECT_EQ(m.ref_count(m.block_table(1)[1]), 2u);
  EXPECT_EQ(m.stats().cow_copies, 0u);
  EXPECT_VALID(m);
}

TEST(Fork, FreeingTheParentLeavesTheChildIntact) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  m.fork(1, 2);
  EXPECT_EQ(m.free(1), Status::Ok);
  EXPECT_EQ(tokens_of(m, 2), iota_tokens(0, 6));
  for (const BlockId b : m.block_table(2)) EXPECT_EQ(m.ref_count(b), 1u);
  EXPECT_EQ(m.stats().used_blocks, 2u);
  EXPECT_VALID(m);
}

TEST(Fork, FreeingTheChildLeavesTheParentIntact) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  m.fork(1, 2);
  m.append(2, Tokens{99});
  EXPECT_EQ(m.free(2), Status::Ok);
  EXPECT_EQ(tokens_of(m, 1), iota_tokens(0, 6));
  EXPECT_EQ(m.stats().used_blocks, 2u);
  EXPECT_VALID(m);
}

TEST(Fork, MistakesAreReportedAndChangeNothing) {
  auto m = make(8);
  m.allocate(1, iota_tokens(0, 6));
  m.allocate(2, iota_tokens(100, 2));
  const auto before = snapshot(m, {1, 2});
  EXPECT_EQ(m.fork(7, 8), Status::UnknownSequence);
  EXPECT_EQ(m.fork(1, 2), Status::DuplicateSequence);
  EXPECT_EQ(snapshot(m, {1, 2}), before);
}

TEST(Fork, ACopyNeedsABlockAndWithoutOneNothingChanges) {
  auto m = make(2);
  m.allocate(1, iota_tokens(0, 6));  // both blocks
  EXPECT_EQ(m.num_available_blocks(), 0u);
  EXPECT_EQ(m.fork(1, 2), Status::Ok);  // sharing needs no new block
  const auto before = snapshot(m, {1, 2});
  EXPECT_EQ(m.append(2, Tokens{9}).status, Status::OutOfBlocks);
  EXPECT_EQ(snapshot(m, {1, 2}), before);
  EXPECT_EQ(m.stats().cow_copies, 0u);
  EXPECT_VALID(m);
}

TEST(Fork, SeveralChildrenDivergeOneCopyAtATime) {
  auto m = make(16);
  m.allocate(1, iota_tokens(0, 6));
  for (SeqId child = 2; child <= 4; ++child) ASSERT_EQ(m.fork(1, child), Status::Ok);
  const BlockId full = m.block_table(1)[0];
  EXPECT_EQ(m.ref_count(full), 4u);
  for (SeqId id = 1; id <= 4; ++id) {
    ASSERT_EQ(m.append(id, Tokens{static_cast<TokenId>(100 + id)}).status, Status::Ok);
    EXPECT_VALID(m);
  }
  EXPECT_EQ(m.stats().cow_copies, 3u);  // the last one to write is alone with the original block
  EXPECT_EQ(m.stats().used_blocks, 5u);
  EXPECT_EQ(m.ref_count(full), 4u);
  for (SeqId id = 1; id <= 4; ++id) {
    EXPECT_EQ(tokens_of(m, id), join({iota_tokens(0, 6), {static_cast<TokenId>(100 + id)}})) << id;
  }
}

TEST(Fork, ChildrenThatFillTheSameBlockTheSameWayCacheItOnce) {
  auto m = make(16);
  m.allocate(1, iota_tokens(0, 6));
  m.fork(1, 2);
  m.append(1, iota_tokens(6, 2));
  m.append(2, iota_tokens(6, 2));  // the same content on the same prefix, built separately
  EXPECT_VALID(m);
  m.free(1);
  m.free(2);
  EXPECT_EQ(m.stats().cached_free_blocks, 2u);  // [0..3] and one copy of [4..7]
  EXPECT_EQ(m.allocate(3, iota_tokens(0, 9)).cached_tokens, 8u);
}

}  // namespace
