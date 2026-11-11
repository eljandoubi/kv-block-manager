# kv-block-manager

[![ci](https://github.com/eljandoubi/kv-block-manager/actions/workflows/ci.yml/badge.svg)](https://github.com/eljandoubi/kv-block-manager/actions/workflows/ci.yml)

A paged KV-cache block manager with prefix caching and copy-on-write, in C++20. It is the
bookkeeping half of PagedAttention-style LLM serving: which fixed-size block of KV cache holds which
tokens of which sequence, which blocks can be shared, which can be reused by a later prompt, and
which must go when the pool is full. No GPU, no dependencies besides the standard library (GoogleTest
for the tests).

- **Block tables.** A sequence is a list of block ids. The pool is handed out one block at a time as
  the sequence grows, so nothing is reserved for a length nobody knows yet.
- **Reference counting.** Blocks can be shared, by forked sequences (parallel sampling, beam search)
  and by prompts with a common start.
- **Prefix caching.** A full block is named by the hash of its tokens *chained to the hash of
  everything before it*, so the same tokens after a different prefix are a different block. A
  released block keeps its content, and a later prompt with the same prefix finds it again.
- **Eviction.** Cached blocks nobody uses are evicted least recently used first, and only when the
  pool has nothing empty left. A sequence is freed tail first, so heads of prompts, the part others are
  likely to share, outlive their tails.
- **Copy-on-write.** A block more than one sequence reads is never written to. Appending to a shared
  partial block hands back a copy order (`src`, `dst`) for the caller to carry out on the KV tensors.
- **All or nothing.** An allocation or an append that cannot finish returns `OutOfBlocks` and changes
  nothing.

## What it buys

`kvbm_simulate` serves 2,000 requests from a queue through a pool of 4,096 blocks of 16 tokens (65,536
tokens of KV cache). Eight shared system prompts of 256 to 2,048 tokens (zipf-distributed), 32 to 256
tokens of the request's own, 64 to 256 generated tokens. A decode step advances every running
sequence by one token; a sequence that cannot get a block is preempted and starts over. The same
workload runs with the cache off and on:

|  | cache off | cache on |
|:--|--:|--:|
| decode steps to finish | 4,869 | **1,518** |
| sequences running per step (mean) | 66.2 | **216.2** |
| prompt tokens computed | 1,976,206 | **431,824** |
| prompt tokens served from cache | 0 | 2,186,880 |
| block hit rate | - | 84.3% |
| preemptions | 334 | 1,093 |
| evictions | 0 | 42,067 |

The shared prefixes are stored once, so three times as many sequences fit and the prompt work drops
to about a fifth. More sequences in flight also means more of them run out of room and restart: the
preemptions go up. This is a queue that is always full, with a made-up workload and no model: it
shows what the bookkeeping allows, not the speed of a real server.

**What the bookkeeping costs** (Release build, Apple M4, one thread, `kvbm_bench`, median of 7 runs):

| operation | time |
|:--|--:|
| allocate + free a 1,024-token prompt (64 blocks), cache off | 972 ns |
| allocate + free the same prompt, 63 of 64 blocks hit | 5.3 µs |
| append one token (decode step) | 22 ns |
| fork + copy-on-write append + free the child | 181 ns |
| hash the 64 chained blocks of a 1,024-token prompt | 2.9 µs |

Most of the extra 4.3 µs with the cache on is hashing. A block is hashed from its own tokens first
and only then mixed with the hash of the block before it, so the CPU can work on several blocks at
once: hashing each block after the previous one's hash was a chain of dependent multiplications and
took 7.3 µs for the same prompt. A prefill of a prompt this long takes milliseconds on a GPU.

## Use

```cpp
#include "kvbm/block_manager.hpp"

kvbm::BlockManager manager({.num_blocks = 4096, .block_size = 16});

std::vector<kvbm::TokenId> prompt = /* ... */;
auto r = manager.allocate(/*id=*/1, prompt);
if (r.status == kvbm::Status::OutOfBlocks) { /* queue it, or preempt something */ }
// the first r.cached_tokens tokens already have their KV data in the blocks of manager.block_table(1)

auto step = manager.append(1, std::span(&next_token, 1));      // one decode step
for (auto [src, dst] : step.copies) { /* copy the KV data of block src into block dst */ }

manager.fork(1, 2);   // parallel sampling: sequence 2 shares everything sequence 1 has
manager.free(1);      // the full blocks stay cached for the next prompt that starts the same way
```

`allocate` never reuses the block that holds the last token of the prompt: the model has to run on
at least one token to get logits, and writing to a shared block would need a copy.

## Rules the code keeps

`BlockManager::check_invariants()` walks everything and returns the first violation, or `""`:

- every block is in exactly one place: on the free list, on the LRU list of cached unused blocks, or
  in use;
- a block's reference count equals the number of times sequences hold it;
- a free block is empty; an LRU block is full, hashed, unused, and the holder of its hash; the hash map
  has no stale entry and no block claims an entry it does not have;
- a sequence has `ceil(tokens / block_size)` blocks, all full but the last; every full block's chain
  hash is the hash of its tokens after the previous one; a partial block is never hashed.

## Tests

97 tests, built with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`:

- unit tests for each rule: sharing, the last-token rule, chaining, eviction order, copy-on-write,
  forks, atomic failure;
- **randomized model tests**: 7 pool shapes (down to 5 blocks, block size 1, blocks larger than the
  prompts, cache off) times 60 seeds times 300 random operations. After every step the invariants
  hold, every sequence reads back exactly its tokens, a refused operation left no trace, and an
  operation is never refused when the blocks were there;
- **collision tests** with hash functions that collide on purpose: a reused block must have the
  same tokens and the same parent hash, not only the same hash;
- **tests of the checker**: each damages one thing inside a healthy manager and expects the checker
  to say what is wrong; every failure it can report has one;
- the whole suite also runs under AddressSanitizer and UndefinedBehaviorSanitizer;
- `scripts/mutate.py` changes one line at a time in a copy of the library (31 mutants: an eviction
  order, a reference count, a missing check, a lost statistic) and runs the tests: all are killed.
  A first version had one survivor, an equivalent change of the hash mixing, which was replaced by a
  mutant that matters. One mutant is caught by a `static_assert` in the tests, which stops them from
  compiling.

Line coverage is 97 percent as GCC's gcov counts it in CI, which fails below 95 (99 percent with
Apple clang's). The lines not run are the unreachable `return "?"` after a `switch` and, for GCC,
exception clean-up paths.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKVBM_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/kvbm_simulate            # the table above
./build/kvbm_bench
python3 scripts/mutate.py        # the mutation check
docker build -t kv-block-manager . && docker run --rm kv-block-manager
```

Options of the build: `KVBM_SANITIZE`, `KVBM_COVERAGE`, `KVBM_WERROR`, `KVBM_BUILD_TESTS`,
`KVBM_BUILD_BENCH`. Tests download GoogleTest with `FetchContent`.

## Limits

- Not thread-safe, and it holds metadata only: moving the KV data is the caller's job.
- The block hash is 64 bits and chained. A block is reused only if its tokens and its parent's hash
  match as well, so two blocks that collide are never confused. Two different prefixes that collide
  *and* are followed by the same tokens would be (at most about 3e-8 for a million cached blocks). For
  untrusted prompts, give `Config::hasher` a keyed hash.
- Only full blocks are cached, only a prefix can be reused, and eviction is plain LRU.
- No sliding-window attention, no cache salt per adapter or tenant, no swap to host memory.

## Related work

The design follows the block manager of vLLM (Kwon et al., *Efficient Memory Management for Large
Language Model Serving with PagedAttention*, SOSP 2023): fixed-size blocks, block tables, reference
counts, copy-on-write, and hash-chained blocks for automatic prefix caching. SGLang's RadixAttention
(Zheng et al., 2023) keeps cached prefixes in a radix tree instead. This is an independent small
implementation written to study the invariants, not a port of either.

## License

Apache-2.0, see [LICENSE](LICENSE).
