#include "kvbm/block_manager.hpp"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <stdexcept>

namespace kvbm {

const char* to_string(Status s) noexcept {
  switch (s) {
    case Status::Ok:
      return "ok";
    case Status::OutOfBlocks:
      return "out of blocks";
    case Status::UnknownSequence:
      return "unknown sequence";
    case Status::DuplicateSequence:
      return "duplicate sequence";
  }
  return "?";
}

BlockManager::BlockManager(Config config) : cfg_(config) {
  if (cfg_.num_blocks == 0) throw std::invalid_argument("num_blocks must be at least 1");
  if (cfg_.block_size == 0) throw std::invalid_argument("block_size must be at least 1");
  if (cfg_.num_blocks >= kNoBlock) throw std::invalid_argument("num_blocks is too large");
  blocks_.resize(cfg_.num_blocks);
  for (Block& b : blocks_) b.tokens.reserve(cfg_.block_size);
  free_.reserve(cfg_.num_blocks);
  // the lowest ids come off the stack first
  for (std::size_t i = cfg_.num_blocks; i > 0; --i) free_.push_back(static_cast<BlockId>(i - 1));
}

std::size_t BlockManager::num_available_blocks() const noexcept {
  return free_.size() + lru_size_;
}

Stats BlockManager::stats() const noexcept {
  Stats s = stats_;
  s.free_blocks = free_.size();
  s.cached_free_blocks = lru_size_;
  s.used_blocks = cfg_.num_blocks - free_.size() - lru_size_;
  return s;
}

void BlockManager::reset(Block& b) {
  b.ref = 0;
  b.hashed = false;
  b.in_map = false;
  b.in_lru = false;
  b.hash = 0;
  b.parent_hash = 0;
  b.tokens.clear();
  b.lru_prev = kNoBlock;
  b.lru_next = kNoBlock;
}

void BlockManager::lru_push_back(BlockId id) {
  Block& b = blocks_[id];
  b.in_lru = true;
  b.lru_prev = lru_tail_;
  b.lru_next = kNoBlock;
  if (lru_tail_ != kNoBlock) {
    blocks_[lru_tail_].lru_next = id;
  } else {
    lru_head_ = id;
  }
  lru_tail_ = id;
  ++lru_size_;
}

void BlockManager::lru_remove(BlockId id) {
  Block& b = blocks_[id];
  if (b.lru_prev != kNoBlock) {
    blocks_[b.lru_prev].lru_next = b.lru_next;
  } else {
    lru_head_ = b.lru_next;
  }
  if (b.lru_next != kNoBlock) {
    blocks_[b.lru_next].lru_prev = b.lru_prev;
  } else {
    lru_tail_ = b.lru_prev;
  }
  b.in_lru = false;
  b.lru_prev = kNoBlock;
  b.lru_next = kNoBlock;
  --lru_size_;
}

BlockId BlockManager::take_block() {
  if (!free_.empty()) {
    const BlockId id = free_.back();
    free_.pop_back();
    return id;
  }
  // callers have checked num_available_blocks(), so something is cached and unused
  const BlockId id = lru_head_;
  lru_remove(id);
  by_hash_.erase(blocks_[id].hash);
  reset(blocks_[id]);
  ++stats_.evictions;
  return id;
}

void BlockManager::release(BlockId id) {
  Block& b = blocks_[id];
  if (--b.ref > 0) return;
  if (b.in_map) {
    lru_push_back(id);  // keep the content: a later prompt may want it
  } else {
    reset(b);
    free_.push_back(id);
  }
}

void BlockManager::acquire_cached(BlockId id) {
  Block& b = blocks_[id];
  if (b.in_lru) lru_remove(id);
  ++b.ref;
}

std::uint64_t BlockManager::hash(std::uint64_t parent, std::span<const TokenId> tokens) const {
  return cfg_.hasher != nullptr ? cfg_.hasher(parent, tokens) : hash_block(parent, tokens);
}

void BlockManager::seal(BlockId id, std::uint64_t parent) {
  if (!cfg_.enable_prefix_cache) return;  // nothing will ever look the block up: skip the hashing
  Block& b = blocks_[id];
  b.hash = hash(parent, b.tokens);
  b.parent_hash = parent;
  b.hashed = true;
  // if another block already holds this hash (same content, built separately) it keeps it
  b.in_map = by_hash_.try_emplace(b.hash, id).second;
}

BlockManager::Plan BlockManager::plan(std::span<const TokenId> prompt) const {
  const std::size_t bs = cfg_.block_size;
  Plan p;
  p.full_blocks = prompt.size() / bs;
  p.total_blocks = (prompt.size() + bs - 1) / bs;
  std::size_t hits_in_lru = 0;
  if (cfg_.enable_prefix_cache && !prompt.empty()) {
    // at least one token has to be computed, so the block holding the last one is never reused
    const std::size_t max_hits = (prompt.size() - 1) / bs;
    std::uint64_t parent = kRootHash;
    for (std::size_t i = 0; i < max_hits; ++i) {
      const auto tokens = prompt.subspan(i * bs, bs);
      const std::uint64_t h = hash(parent, tokens);
      const auto found = by_hash_.find(h);
      if (found == by_hash_.end()) break;
      const Block& b = blocks_[found->second];
      // trust the tokens, not only the hash
      if (b.parent_hash != parent || !std::ranges::equal(b.tokens, tokens)) break;
      p.hits.push_back(found->second);
      if (b.ref == 0) ++hits_in_lru;
      parent = h;
    }
  }
  // a hit that sits in the LRU list is available now, but not once we have claimed it
  p.fits = p.total_blocks - p.hits.size() <= num_available_blocks() - hits_in_lru;
  return p;
}

bool BlockManager::can_allocate(std::span<const TokenId> prompt) const {
  return plan(prompt).fits;
}

AllocResult BlockManager::allocate(SeqId id, std::span<const TokenId> prompt) {
  if (seqs_.contains(id)) return {.status = Status::DuplicateSequence};
  const Plan p = plan(prompt);
  if (!p.fits) {
    ++stats_.failed_requests;
    return {.status = Status::OutOfBlocks};
  }
  const std::size_t bs = cfg_.block_size;
  Sequence seq;
  seq.table.reserve(p.total_blocks);
  std::uint64_t parent = kRootHash;
  for (const BlockId b : p.hits) {
    acquire_cached(b);
    seq.table.push_back(b);
    parent = blocks_[b].hash;
  }
  for (std::size_t pos = p.hits.size() * bs; pos < prompt.size(); pos += bs) {
    const auto chunk = prompt.subspan(pos, std::min(bs, prompt.size() - pos));
    const BlockId b = take_block();
    Block& blk = blocks_[b];
    blk.ref = 1;
    blk.tokens.assign(chunk.begin(), chunk.end());
    if (chunk.size() == bs) {
      seal(b, parent);
      parent = blk.hash;
    }
    seq.table.push_back(b);
  }
  seq.num_tokens = prompt.size();
  seq.last_full_hash = parent;
  stats_.lookup_blocks += p.full_blocks;
  stats_.hit_blocks += p.hits.size();
  seqs_.emplace(id, std::move(seq));
  return {.status = Status::Ok, .cached_tokens = p.hits.size() * bs};
}

AppendResult BlockManager::append(SeqId id, std::span<const TokenId> tokens) {
  const auto it = seqs_.find(id);
  if (it == seqs_.end()) return {.status = Status::UnknownSequence, .copies = {}};
  if (tokens.empty()) return {};
  Sequence& seq = it->second;
  const std::size_t bs = cfg_.block_size;

  const std::size_t room = (bs - seq.num_tokens % bs) % bs;  // free slots in the last block
  const bool cow = room > 0 && blocks_[seq.table.back()].ref > 1;
  const std::size_t extra = tokens.size() > room ? tokens.size() - room : 0;
  const std::size_t needed = (extra + bs - 1) / bs + (cow ? 1 : 0);
  if (needed > num_available_blocks()) {
    ++stats_.failed_requests;
    return {.status = Status::OutOfBlocks, .copies = {}};
  }

  AppendResult result;
  if (cow) {
    // someone else still reads the last block: write into a copy of it
    const BlockId shared = seq.table.back();
    const BlockId copy = take_block();
    blocks_[copy].ref = 1;
    blocks_[copy].tokens = blocks_[shared].tokens;
    --blocks_[shared].ref;  // it was shared, so it is still in use
    seq.table.back() = copy;
    result.copies.push_back({.src = shared, .dst = copy});
    ++stats_.cow_copies;
  }
  for (const TokenId t : tokens) {
    if (seq.num_tokens % bs == 0) {
      const BlockId b = take_block();
      blocks_[b].ref = 1;
      seq.table.push_back(b);
    }
    Block& last = blocks_[seq.table.back()];
    last.tokens.push_back(t);
    ++seq.num_tokens;
    if (last.tokens.size() == bs) {
      seal(seq.table.back(), seq.last_full_hash);
      seq.last_full_hash = last.hash;
    }
  }
  return result;
}

Status BlockManager::fork(SeqId parent, SeqId child) {
  const auto it = seqs_.find(parent);
  if (it == seqs_.end()) return Status::UnknownSequence;
  if (seqs_.contains(child)) return Status::DuplicateSequence;
  Sequence copy = it->second;  // before emplace: it may rehash the map
  for (const BlockId b : copy.table) ++blocks_[b].ref;
  seqs_.emplace(child, std::move(copy));
  return Status::Ok;
}

Status BlockManager::free(SeqId id) {
  const auto it = seqs_.find(id);
  if (it == seqs_.end()) return Status::UnknownSequence;
  // last block first: when the pool runs dry the tail of a prompt goes before its head, because
  // heads are the part other prompts are most likely to share
  const std::vector<BlockId>& table = it->second.table;
  for (const BlockId b : std::views::reverse(table)) release(b);
  seqs_.erase(it);
  return Status::Ok;
}

std::span<const BlockId> BlockManager::block_table(SeqId id) const {
  const auto it = seqs_.find(id);
  return it == seqs_.end() ? std::span<const BlockId>{} : std::span<const BlockId>(it->second.table);
}

std::size_t BlockManager::num_tokens(SeqId id) const {
  const auto it = seqs_.find(id);
  return it == seqs_.end() ? 0 : it->second.num_tokens;
}

std::span<const TokenId> BlockManager::block_tokens(BlockId id) const {
  return id < blocks_.size() ? std::span<const TokenId>(blocks_[id].tokens) : std::span<const TokenId>{};
}

std::size_t BlockManager::ref_count(BlockId id) const {
  return id < blocks_.size() ? blocks_[id].ref : 0;
}

std::string BlockManager::check_invariants() const {
  std::ostringstream err;
  const auto fail = [&err](const auto&... parts) {
    (err << ... << parts);
    return err.str();
  };
  const std::size_t n = blocks_.size();
  const std::size_t bs = cfg_.block_size;

  // every block is in exactly one place: the free list, the LRU list, or in use
  enum Where : char { kNowhere = 0, kFree = 1, kLru = 2 };
  std::vector<char> where(n, kNowhere);
  for (const BlockId b : free_) {
    if (b >= n) return fail("free list holds the unknown block ", b);
    if (where[b] != kNowhere) return fail("block ", b, " is on the free list twice");
    where[b] = kFree;
    const Block& blk = blocks_[b];
    if (blk.ref != 0 || blk.hashed || blk.in_map || blk.in_lru || !blk.tokens.empty()) {
      return fail("free block ", b, " is not empty");
    }
  }
  std::size_t listed = 0;
  BlockId before = kNoBlock;
  for (BlockId b = lru_head_; b != kNoBlock; b = blocks_[b].lru_next) {
    if (b >= n || where[b] != kNowhere) return fail("LRU list holds block ", b, " twice or on the free list");
    where[b] = kLru;
    ++listed;
    const Block& blk = blocks_[b];
    if (!blk.in_lru || blk.ref != 0 || !blk.hashed || !blk.in_map)
      return fail("LRU block ", b, " is not cached and unused");
    if (blk.lru_prev != before) return fail("LRU block ", b, " has a broken back link");
    const auto held = by_hash_.find(blk.hash);
    if (held == by_hash_.end() || held->second != b) return fail("LRU block ", b, " is not the holder of its hash");
    before = b;
  }
  if (before != lru_tail_) return fail("LRU tail is wrong");
  if (listed != lru_size_) return fail("LRU size says ", lru_size_, " but the list has ", listed);

  // the hash map and the flags agree
  for (const auto& [hash, b] : by_hash_) {
    if (b >= n) return fail("hash map points to the unknown block ", b);
    const Block& blk = blocks_[b];
    if (!blk.in_map || !blk.hashed || blk.hash != hash) return fail("hash map entry for block ", b, " is stale");
  }
  std::size_t flagged = 0;
  for (const Block& blk : blocks_) flagged += blk.in_map ? 1 : 0;
  if (flagged != by_hash_.size())
    return fail("hash map has ", by_hash_.size(), " entries but ", flagged, " blocks claim one");

  // reference counts add up, and every sequence is what its tokens say
  std::vector<std::size_t> refs(n, 0);
  for (const auto& [id, seq] : seqs_) {
    if (seq.table.size() != (seq.num_tokens + bs - 1) / bs)
      return fail("sequence ", id, " has the wrong number of blocks");
    std::uint64_t parent = kRootHash;
    for (std::size_t i = 0; i < seq.table.size(); ++i) {
      const BlockId b = seq.table[i];
      if (b >= n) return fail("sequence ", id, " holds the unknown block ", b);
      ++refs[b];
      const Block& blk = blocks_[b];
      const std::size_t expected = i + 1 == seq.table.size() ? seq.num_tokens - i * bs : bs;
      if (blk.tokens.size() != expected)
        return fail("block ", b, " of sequence ", id, " holds ", blk.tokens.size(), " tokens, not ", expected);
      if (expected == bs && cfg_.enable_prefix_cache) {
        if (!blk.hashed || blk.parent_hash != parent || blk.hash != hash(parent, blk.tokens)) {
          return fail("block ", b, " of sequence ", id, " has a wrong chain hash");
        }
        parent = blk.hash;
      } else if (blk.hashed || blk.in_map) {
        return fail("partial block ", b, " of sequence ", id, " is hashed");
      }
    }
    if (cfg_.enable_prefix_cache && seq.last_full_hash != parent)
      return fail("sequence ", id, " has a wrong last hash");
  }
  for (std::size_t b = 0; b < n; ++b) {
    if (refs[b] != blocks_[b].ref)
      return fail("block ", b, " says ", blocks_[b].ref, " references, sequences hold ", refs[b]);
    if (blocks_[b].ref == 0 && where[b] == kNowhere) return fail("block ", b, " is lost: unused and nowhere");
  }
  return "";
}

}  // namespace kvbm
