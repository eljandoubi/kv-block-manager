#include "kvbm/hash.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

using kvbm::hash_block;
using kvbm::kRootHash;
using kvbm::mix64;
using kvbm::TokenId;

namespace {

TEST(Hash, SameInputsGiveTheSameHash) {
  const std::vector<TokenId> t = {1, 2, 3, 4};
  EXPECT_EQ(hash_block(kRootHash, t), hash_block(kRootHash, t));
}

TEST(Hash, ItIsUsableAtCompileTime) {
  constexpr TokenId tokens[] = {7, 8, 9};
  static_assert(hash_block(kRootHash, tokens) == hash_block(kRootHash, tokens));
  static_assert(hash_block(kRootHash, tokens) != hash_block(kRootHash + 1, tokens));
  SUCCEED();
}

TEST(Hash, OrderMatters) {
  const std::vector<TokenId> a = {1, 2, 3, 4};
  const std::vector<TokenId> b = {4, 3, 2, 1};
  EXPECT_NE(hash_block(kRootHash, a), hash_block(kRootHash, b));
}

TEST(Hash, ThePrefixMatters) {
  const std::vector<TokenId> t = {5, 6, 7, 8};
  EXPECT_NE(hash_block(1, t), hash_block(2, t));
}

TEST(Hash, LengthMatters) {
  const std::vector<TokenId> zeros2 = {0, 0};
  const std::vector<TokenId> zeros3 = {0, 0, 0};
  EXPECT_NE(hash_block(kRootHash, zeros2), hash_block(kRootHash, zeros3));
  EXPECT_NE(hash_block(kRootHash, {}), hash_block(kRootHash, zeros2));
}

TEST(Hash, NegativeTokensAreFine) {
  const std::vector<TokenId> a = {-1, 5};
  const std::vector<TokenId> b = {1, 5};
  EXPECT_NE(hash_block(kRootHash, a), hash_block(kRootHash, b));
}

TEST(Hash, ChainingMakesAPrefixPartOfTheIdentity) {
  // the second block has the same tokens after two different first blocks
  const std::vector<TokenId> first_a = {1, 2}, first_b = {3, 4}, second = {9, 9};
  const auto ha = hash_block(hash_block(kRootHash, first_a), second);
  const auto hb = hash_block(hash_block(kRootHash, first_b), second);
  EXPECT_NE(ha, hb);
}

TEST(Hash, FlippingOneBitOfOneTokenChangesAboutHalfTheOutputBits) {
  std::mt19937_64 rng(1);
  double flipped = 0;
  const int trials = 2000;
  for (int i = 0; i < trials; ++i) {
    std::vector<TokenId> t(16);
    for (auto& x : t) x = static_cast<TokenId>(rng());
    const auto before = hash_block(kRootHash, t);
    t[rng() % t.size()] ^= static_cast<TokenId>(1u << (rng() % 31));
    flipped += std::popcount(before ^ hash_block(kRootHash, t));
  }
  EXPECT_NEAR(flipped / trials, 32.0, 1.5);
}

TEST(Hash, NoCollisionsAmongAHundredThousandRandomBlocks) {
  std::mt19937_64 rng(2);
  std::unordered_set<std::uint64_t> seen;
  for (int i = 0; i < 100000; ++i) {
    std::vector<TokenId> t(16);
    for (auto& x : t) x = static_cast<TokenId>(rng() % 50000);
    seen.insert(hash_block(kRootHash, t));
  }
  EXPECT_EQ(seen.size(), 100000u);
}

TEST(Hash, EverySmallBlockOverASmallVocabularyIsDistinct) {
  // all 4-token blocks over 8 token values: 4096 blocks, and the same under another prefix
  std::unordered_set<std::uint64_t> seen;
  for (TokenId a = 0; a < 8; ++a)
    for (TokenId b = 0; b < 8; ++b)
      for (TokenId c = 0; c < 8; ++c)
        for (TokenId d = 0; d < 8; ++d) {
          const std::vector<TokenId> t = {a, b, c, d};
          seen.insert(hash_block(kRootHash, t));
          seen.insert(hash_block(12345, t));
        }
  EXPECT_EQ(seen.size(), 2u * 4096u);
}

TEST(Mix, IsABijectionOnASample) {
  std::unordered_set<std::uint64_t> seen;
  for (std::uint64_t i = 0; i < 100000; ++i) seen.insert(mix64(i));
  EXPECT_EQ(seen.size(), 100000u);
}

}  // namespace
