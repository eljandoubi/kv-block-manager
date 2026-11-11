// Random operations against a small pool, checked after every step against a plain model of what
// each sequence should hold and against the manager's own invariants.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>

#include "helpers.hpp"

using namespace kvbm;
using namespace kvbm::testing;

namespace {

struct Params {
  std::size_t blocks;
  std::size_t block_size;
  bool cache;
  unsigned vocab;  // few distinct tokens make repeated blocks likely
};

std::size_t ceil_div(std::size_t a, std::size_t b) {
  return (a + b - 1) / b;
}

class Fuzzer {
 public:
  Fuzzer(const Params& p, std::uint64_t seed)
      : p_(p),
        rng_(seed),
        m_(Config{.num_blocks = p.blocks, .block_size = p.block_size, .enable_prefix_cache = p.cache}) {
    for (int i = 0; i < 3; ++i) templates_.push_back(random_tokens(4 * p.block_size));
  }

  void run(int steps) {
    for (int step = 0; step < steps; ++step) {
      SCOPED_TRACE(::testing::Message() << "step " << step);
      do_one_operation();
      ASSERT_EQ(m_.check_invariants(), "");
      check_contents();
      if (::testing::Test::HasFatalFailure()) return;
    }
    finish();
  }

 private:
  Tokens random_tokens(std::size_t n) {
    Tokens t(n);
    for (auto& x : t) x = static_cast<TokenId>(rng_() % p_.vocab);
    return t;
  }

  std::vector<SeqId> live() const {
    std::vector<SeqId> ids;
    for (const auto& [id, tokens] : model_) ids.push_back(id);
    return ids;
  }

  SeqId pick() {
    const auto ids = live();
    return ids[rng_() % ids.size()];
  }

  Tokens random_prompt() {
    Tokens prompt;
    if (rng_() % 4 != 0) {  // most prompts start like one of a few templates
      const Tokens& t = templates_[rng_() % templates_.size()];
      prompt.assign(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(rng_() % (t.size() + 1)));
    }
    const Tokens tail = random_tokens(rng_() % (2 * p_.block_size + 2));
    prompt.insert(prompt.end(), tail.begin(), tail.end());
    return prompt;
  }

  void do_one_operation() {
    const auto before = snapshot(m_, live());
    const unsigned op = static_cast<unsigned>(rng_() % 100);
    if (op < 25 || model_.empty()) {
      allocate(before);
    } else if (op < 65) {
      append(before);
    } else if (op < 75) {
      fork();
    } else if (op < 95) {
      release();
    } else {
      (void)m_.can_allocate(random_prompt());
      EXPECT_EQ(snapshot(m_, live()), before);  // asking changes nothing
    }
  }

  void allocate(const Snapshot& before) {
    const Tokens prompt = random_prompt();
    const SeqId id = next_++;
    const std::size_t available = m_.num_available_blocks();
    const bool predicted = m_.can_allocate(prompt);
    const AllocResult r = m_.allocate(id, prompt);
    if (r.status == Status::Ok) {
      EXPECT_TRUE(predicted);
      model_[id] = prompt;
      EXPECT_EQ(r.cached_tokens % p_.block_size, 0u);
      if (prompt.empty()) {
        EXPECT_EQ(r.cached_tokens, 0u);
      } else {
        EXPECT_LT(r.cached_tokens, prompt.size());
      }
      if (!p_.cache) EXPECT_EQ(r.cached_tokens, 0u);
      for (std::size_t i = 0; i < r.cached_tokens / p_.block_size; ++i) {  // reused blocks hold the prompt's own tokens
        const auto held = m_.block_tokens(m_.block_table(id)[i]);
        EXPECT_TRUE(
            std::equal(held.begin(), held.end(), prompt.begin() + static_cast<std::ptrdiff_t>(i * p_.block_size)));
      }
    } else {
      ASSERT_EQ(r.status, Status::OutOfBlocks);
      EXPECT_FALSE(predicted);
      EXPECT_LT(available, ceil_div(prompt.size(), p_.block_size));  // with that many blocks it could not fail
      EXPECT_EQ(snapshot(m_, live()), before);
      EXPECT_EQ(m_.num_sequences(), model_.size());
    }
  }

  void append(const Snapshot& before) {
    const SeqId id = pick();
    const Tokens tokens = random_tokens(1 + rng_() % (2 * p_.block_size + 1));
    const std::size_t available = m_.num_available_blocks();
    const AppendResult r = m_.append(id, tokens);
    if (r.status == Status::Ok) {
      model_[id].insert(model_[id].end(), tokens.begin(), tokens.end());
      for (const CopyOp& c : r.copies) {
        EXPECT_NE(c.src, c.dst);
        EXPECT_GE(m_.ref_count(c.src), 1u);  // somebody still reads the original
        const auto from = m_.block_tokens(c.src);
        const auto to = m_.block_tokens(c.dst);
        ASSERT_LE(from.size(), to.size());
        EXPECT_TRUE(std::equal(from.begin(), from.end(), to.begin()));  // the copy starts as the original
      }
    } else {
      ASSERT_EQ(r.status, Status::OutOfBlocks);
      EXPECT_TRUE(r.copies.empty());
      // ceil(n / bs) blocks plus one for a copy are always enough
      EXPECT_LT(available, ceil_div(tokens.size(), p_.block_size) + 1);
      EXPECT_EQ(snapshot(m_, live()), before);
    }
  }

  void fork() {
    const SeqId parent = pick();
    const SeqId child = next_++;
    ASSERT_EQ(m_.fork(parent, child), Status::Ok);
    model_[child] = model_[parent];
    EXPECT_EQ(m_.fork(parent, child), Status::DuplicateSequence);
  }

  void release() {
    const SeqId id = pick();
    ASSERT_EQ(m_.free(id), Status::Ok);
    model_.erase(id);
    EXPECT_EQ(m_.free(id), Status::UnknownSequence);
  }

  void check_contents() {
    for (const auto& [id, tokens] : model_) {
      ASSERT_EQ(tokens_of(m_, id), tokens) << "sequence " << id;
      ASSERT_EQ(m_.num_tokens(id), tokens.size());
    }
    const Stats s = m_.stats();
    ASSERT_EQ(s.used_blocks + s.cached_free_blocks + s.free_blocks, p_.blocks);
  }

  void finish() {
    for (const SeqId id : live()) ASSERT_EQ(m_.free(id), Status::Ok);
    EXPECT_EQ(m_.stats().used_blocks, 0u);
    EXPECT_EQ(m_.num_available_blocks(), p_.blocks);
    EXPECT_EQ(m_.check_invariants(), "");
  }

  Params p_;
  std::mt19937_64 rng_;
  BlockManager m_;
  std::map<SeqId, Tokens> model_;
  std::vector<Tokens> templates_;
  SeqId next_ = 1;
};

class Randomized : public ::testing::TestWithParam<Params> {};

TEST_P(Randomized, EveryOperationKeepsTheInvariantsAndEverySequenceItsTokens) {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    SCOPED_TRACE(::testing::Message() << "seed " << seed);
    Fuzzer fuzzer(GetParam(), seed);
    fuzzer.run(300);
    if (::testing::Test::HasFatalFailure()) return;
  }
}

INSTANTIATE_TEST_SUITE_P(Pools, Randomized,
                         ::testing::Values(Params{8, 2, true, 3},                            // tiny: constant eviction
                                           Params{16, 4, true, 4}, Params{32, 4, false, 4},  // cache off
                                           Params{6, 3, true, 2},  // two token values: many repeated blocks
                                           Params{64, 8, true, 6}, Params{12, 1, true, 3},  // block size one
                                           Params{5, 16, true, 5}),  // blocks bigger than most prompts
                         [](const ::testing::TestParamInfo<Params>& info) {
                           return "blocks" + std::to_string(info.param.blocks) + "_size" +
                                  std::to_string(info.param.block_size) + (info.param.cache ? "_cache" : "_nocache");
                         });

}  // namespace
