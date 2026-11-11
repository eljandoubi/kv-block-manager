// Nanoseconds per operation for the main paths of the block manager.
//
//   kvbm_bench
//
// Single thread, one pool, wall-clock time of steady_clock around each loop. A row is the time of
// one pass of the body named in the row, except "append", which is per token. These numbers are for
// the bookkeeping only: there is no KV data to move.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "kvbm/block_manager.hpp"

namespace {

using kvbm::BlockManager;
using kvbm::Config;
using kvbm::SeqId;
using kvbm::TokenId;

template <class T>
inline void do_not_optimize(const T& value) {
#if defined(__clang__)
  asm volatile("" : : "g"(value) : "memory");
#else
  asm volatile("" : : "i,r,m"(value) : "memory");
#endif
}

struct Row {
  const char* name;
  std::size_t operations;
  double ns_per_op;
};

template <class F>
Row measure(const char* name, std::size_t iterations, std::size_t ops_per_iteration, F body) {
  body();  // warm up the caches and the allocator
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) body();
  const auto stop = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(stop - start).count();
  const std::size_t ops = iterations * ops_per_iteration;
  return {.name = name, .operations = ops, .ns_per_op = ns / static_cast<double>(ops)};
}

std::vector<TokenId> tokens(std::size_t n, TokenId base = 0) {
  std::vector<TokenId> t(n);
  for (std::size_t i = 0; i < n; ++i) t[i] = base + static_cast<TokenId>(i);
  return t;
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape): a failed allocation may end the benchmark
  constexpr std::size_t kBlockSize = 16;
  std::vector<Row> rows;

  {
    const auto prompt = tokens(1024);
    BlockManager m(Config{.num_blocks = 256, .block_size = kBlockSize, .enable_prefix_cache = false});
    rows.push_back(measure("allocate + free of a 1024-token prompt, cache off", 20000, 1, [&] {
      do_not_optimize(m.allocate(1, prompt));
      do_not_optimize(m.free(1));
    }));
  }
  {
    const auto prompt = tokens(1024);
    BlockManager m(Config{.num_blocks = 256, .block_size = kBlockSize, .enable_prefix_cache = true});
    m.allocate(0, prompt);  // keeps the blocks in use, so every later allocate hits
    rows.push_back(measure("allocate + free of the same prompt, 63 of 64 blocks hit", 20000, 1, [&] {
      do_not_optimize(m.allocate(1, prompt));
      do_not_optimize(m.free(1));
    }));
  }
  {
    BlockManager m(Config{.num_blocks = 256, .block_size = kBlockSize});
    const auto prompt = tokens(64);
    const std::vector<TokenId> one = {7};
    rows.push_back(measure("append one token (decode step)", 2000, 192, [&] {
      m.allocate(1, prompt);
      for (int i = 0; i < 192; ++i) do_not_optimize(m.append(1, one));
      m.free(1);
    }));
  }
  {
    BlockManager m(Config{.num_blocks = 256, .block_size = kBlockSize});
    const auto prompt = tokens(500);  // ends in a partial block
    const std::vector<TokenId> one = {7};
    m.allocate(1, prompt);
    rows.push_back(measure("fork + copy-on-write append + free child", 100000, 1, [&] {
      m.fork(1, 2);
      do_not_optimize(m.append(2, one));
      m.free(2);
    }));
  }
  {
    const auto prompt = tokens(1024);
    rows.push_back(measure("hash the 64 chained blocks of a 1024-token prompt", 200000, 1, [&] {
      std::uint64_t h = kvbm::kRootHash;
      for (std::size_t b = 0; b < 64; ++b) {
        h = kvbm::hash_block(h, std::span<const TokenId>(prompt).subspan(b * kBlockSize, kBlockSize));
      }
      do_not_optimize(h);
    }));
  }

  std::printf("%-52s %12s %10s\n", "operation", "ns / op", "M ops/s");
  for (const Row& r : rows) std::printf("%-52s %12.1f %10.2f\n", r.name, r.ns_per_op, 1000.0 / r.ns_per_op);
  return EXIT_SUCCESS;
}
