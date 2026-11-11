#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "kvbm/hash.hpp"

namespace kvbm {

using BlockId = std::uint32_t;
using SeqId = std::uint64_t;

inline constexpr BlockId kNoBlock = std::numeric_limits<BlockId>::max();

enum class Status : std::uint8_t {
  Ok,
  OutOfBlocks,  // nothing was changed; try again after freeing a sequence
  UnknownSequence,
  DuplicateSequence,
};

const char* to_string(Status s) noexcept;

struct BlockManagerTestAccess;  // lets the tests damage the state on purpose

// Hash of a block of tokens that follows a prefix whose hash is `parent`.
using HashFn = std::uint64_t (*)(std::uint64_t parent, std::span<const TokenId> tokens);

struct Config {
  std::size_t num_blocks = 0;
  std::size_t block_size = 16;  // tokens per block
  bool enable_prefix_cache = true;
  // Replaces kvbm::hash_block. The manager does not trust the hash alone: a cached block is only
  // reused when its tokens and the hash of its parent match too. A different function is useful to
  // test that, and to use a keyed or cryptographic hash when prompts come from untrusted users.
  HashFn hasher = nullptr;
};

// The manager only does the bookkeeping. When it tells you to copy a block, the KV data of `src`
// must be copied into `dst` before anything is written to `dst`.
struct CopyOp {
  BlockId src;
  BlockId dst;
  bool operator==(const CopyOp&) const = default;
};

struct AllocResult {
  Status status = Status::Ok;
  // Leading prompt tokens whose KV data is already in the cache: a multiple of the block size, and
  // always short of the whole prompt, because the last token has to be computed to get its logits.
  std::size_t cached_tokens = 0;
};

struct AppendResult {
  Status status = Status::Ok;
  std::vector<CopyOp> copies;  // copy-on-write copies to make before writing the new tokens
};

struct Stats {
  std::uint64_t lookup_blocks = 0;  // full prompt blocks that could have been found in the cache
  std::uint64_t hit_blocks = 0;     // ... and the ones that were
  std::uint64_t evictions = 0;      // cached blocks thrown away to make room
  std::uint64_t cow_copies = 0;
  std::uint64_t failed_requests = 0;   // allocate or append refused for lack of blocks
  std::size_t used_blocks = 0;         // referenced by at least one sequence
  std::size_t cached_free_blocks = 0;  // referenced by none, kept because their content may be reused
  std::size_t free_blocks = 0;         // empty

  [[nodiscard]] double hit_rate() const noexcept {
    return lookup_blocks == 0 ? 0.0 : static_cast<double>(hit_blocks) / static_cast<double>(lookup_blocks);
  }
};

// A pool of fixed-size KV-cache blocks, handed out to sequences as block tables.
//
//  * Blocks are reference counted, so sequences can share them.
//  * A block that is full gets the hash of its tokens chained to the hash of the blocks before it.
//    When a block is released its content is kept, and a later prompt with the same prefix finds it
//    again (prefix caching). Cached blocks nobody references are evicted least recently used first,
//    and only when the pool has nothing else to give.
//  * A block that more than one sequence references is never written to: appending to a shared
//    partial block first copies it (copy-on-write).
//  * Every operation that cannot finish changes nothing.
//
// Not thread-safe.
class BlockManager {
 public:
  explicit BlockManager(Config config);  // throws std::invalid_argument for a size of zero

  // Start a sequence with `prompt`.
  AllocResult allocate(SeqId id, std::span<const TokenId> prompt);

  // Add tokens to the end of a sequence (decoding, or the next chunk of a long prompt).
  AppendResult append(SeqId id, std::span<const TokenId> tokens);

  // `child` starts out sharing every block of `parent` (parallel sampling, beam search).
  Status fork(SeqId parent, SeqId child);

  // End a sequence. Its blocks go back to the pool; the full ones stay cached.
  Status free(SeqId id);

  // Would `allocate` succeed right now?
  [[nodiscard]] bool can_allocate(std::span<const TokenId> prompt) const;

  // Blocks that can be handed out right now: the empty ones and the cached ones nobody uses.
  [[nodiscard]] std::size_t num_available_blocks() const noexcept;
  [[nodiscard]] std::size_t num_sequences() const noexcept { return seqs_.size(); }

  [[nodiscard]] std::span<const BlockId> block_table(SeqId id) const;
  [[nodiscard]] std::size_t num_tokens(SeqId id) const;
  [[nodiscard]] std::span<const TokenId> block_tokens(BlockId id) const;
  [[nodiscard]] std::size_t ref_count(BlockId id) const;

  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }

  // Walks every structure and checks what must always hold. Returns "" when all is well, else a
  // description of the first problem. Meant for tests and debugging; it costs O(pool size).
  [[nodiscard]] std::string check_invariants() const;

 private:
  friend struct BlockManagerTestAccess;

  struct Block {
    std::uint32_t ref = 0;
    bool hashed = false;  // full, and its chain hash is set
    bool in_map = false;  // registered as the holder of its hash
    bool in_lru = false;
    std::uint64_t hash = 0;
    std::uint64_t parent_hash = 0;
    std::vector<TokenId> tokens;
    BlockId lru_prev = kNoBlock;
    BlockId lru_next = kNoBlock;
  };

  struct Sequence {
    std::vector<BlockId> table;
    std::size_t num_tokens = 0;
    std::uint64_t last_full_hash = kRootHash;  // chain hash after the last full block
  };

  struct Plan {
    std::vector<BlockId> hits;     // cached blocks the prompt can reuse, in order
    std::size_t full_blocks = 0;   // full blocks in the prompt
    std::size_t total_blocks = 0;  // blocks the prompt occupies
    bool fits = false;
  };

  [[nodiscard]] Plan plan(std::span<const TokenId> prompt) const;
  BlockId take_block();             // an empty block, evicting if need be
  void release(BlockId id);         // one reference fewer
  void acquire_cached(BlockId id);  // one reference more, off the LRU list if it was on it
  void seal(BlockId id, std::uint64_t parent);
  [[nodiscard]] std::uint64_t hash(std::uint64_t parent, std::span<const TokenId> tokens) const;
  void reset(Block& b);
  void lru_push_back(BlockId id);
  void lru_remove(BlockId id);

  Config cfg_;
  std::vector<Block> blocks_;
  std::vector<BlockId> free_;
  BlockId lru_head_ = kNoBlock;  // least recently released
  BlockId lru_tail_ = kNoBlock;
  std::size_t lru_size_ = 0;
  std::unordered_map<std::uint64_t, BlockId> by_hash_;
  std::unordered_map<SeqId, Sequence> seqs_;
  Stats stats_;
};

}  // namespace kvbm
