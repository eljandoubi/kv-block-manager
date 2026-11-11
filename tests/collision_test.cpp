// The manager does not trust the hash alone. These tests give it hash functions that collide on
// purpose and check that content is never shared when it is not the same.

#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace kvbm;
using namespace kvbm::testing;

namespace {

// ignores the last token of every block, so blocks that differ only there collide
std::uint64_t ignores_last_token(std::uint64_t parent, std::span<const TokenId> tokens) {
  return hash_block(parent, tokens.first(tokens.empty() ? 0 : tokens.size() - 1));
}

// ignores the prefix, so the same tokens after different prefixes collide
std::uint64_t ignores_prefix(std::uint64_t, std::span<const TokenId> tokens) {
  return hash_block(kRootHash, tokens);
}

std::uint64_t constant(std::uint64_t, std::span<const TokenId>) {
  return 7;
}

BlockManager make(HashFn fn, std::size_t blocks = 16) {
  return BlockManager(Config{.num_blocks = blocks, .block_size = 4, .enable_prefix_cache = true, .hasher = fn});
}

TEST(Collision, ABlockWithTheSameHashButOtherTokensIsNotShared) {
  auto m = make(ignores_last_token);
  m.allocate(1, join({{0, 1, 2, 3}, {9}}));
  const auto r = m.allocate(2, join({{0, 1, 2, 7}, {9}}));  // same hash for the first block
  EXPECT_EQ(r.cached_tokens, 0u);
  EXPECT_NE(m.block_table(1)[0], m.block_table(2)[0]);
  EXPECT_EQ(tokens_of(m, 1), join({{0, 1, 2, 3}, {9}}));
  EXPECT_EQ(tokens_of(m, 2), join({{0, 1, 2, 7}, {9}}));
  EXPECT_VALID(m);
}

TEST(Collision, TheCollidingBlockDoesNotDisplaceTheFirstHolder) {
  auto m = make(ignores_last_token);
  m.allocate(1, join({{0, 1, 2, 3}, {9}}));
  m.allocate(2, join({{0, 1, 2, 7}, {9}}));
  m.free(2);  // the second block was never registered, so it does not become cached
  EXPECT_EQ(m.stats().cached_free_blocks, 0u);
  EXPECT_EQ(m.allocate(3, join({{0, 1, 2, 3}, {9}})).cached_tokens, 4u);  // the holder still works
  EXPECT_VALID(m);
}

TEST(Collision, TheSameTokensAfterAPrefixTheHasherIgnoresAreNotShared) {
  auto m = make(ignores_prefix);
  const Tokens a = {1, 1, 1, 1}, a2 = {2, 2, 2, 2}, b = {5, 5, 5, 5}, x = {8, 8, 8, 8};
  m.allocate(1, join({a, b, {9}}));
  m.allocate(2, join({a2, x, {9}}));  // makes a2 a cached first block
  // a2 is found, but the block [5 5 5 5] that the hash points to follows `a`, not `a2`
  const auto r = m.allocate(3, join({a2, b, {9}}));
  EXPECT_EQ(r.cached_tokens, 4u);
  EXPECT_EQ(m.block_table(3)[0], m.block_table(2)[0]);
  EXPECT_NE(m.block_table(3)[1], m.block_table(1)[1]);
  EXPECT_VALID(m);
}

TEST(Collision, AHashThatAlwaysCollidesStillKeepsEverySequenceIntact) {
  auto m = make(constant, 64);
  std::vector<Tokens> prompts;
  for (TokenId s = 0; s < 8; ++s) prompts.push_back(join({iota_tokens(s, 6), iota_tokens(s * 7, 3)}));
  for (std::size_t i = 0; i < prompts.size(); ++i) {
    ASSERT_EQ(m.allocate(i + 1, prompts[i]).status, Status::Ok);
    EXPECT_VALID(m);
  }
  for (std::size_t i = 0; i < prompts.size(); ++i) EXPECT_EQ(tokens_of(m, i + 1), prompts[i]) << i;
  for (std::size_t i = 0; i < prompts.size(); ++i) m.free(i + 1);
  EXPECT_VALID(m);
}

TEST(Collision, TheDefaultHasherIsUsedWhenNoneIsGiven) {
  BlockManager with_default(Config{.num_blocks = 8, .block_size = 4});
  BlockManager explicit_default(Config{.num_blocks = 8, .block_size = 4, .hasher = &hash_block});
  for (auto* m : {&with_default, &explicit_default}) {
    m->allocate(1, iota_tokens(0, 9));
    EXPECT_EQ(m->allocate(2, iota_tokens(0, 9)).cached_tokens, 8u);
    EXPECT_VALID(*m);
  }
}

}  // namespace
