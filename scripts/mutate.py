#!/usr/bin/env python3
"""Mutation check: change one line of the library at a time and see whether the tests notice.

A surviving mutant is a behaviour no test pins down. Run it from the repository root:

    python3 scripts/mutate.py                 # all mutants
    python3 scripts/mutate.py M03 M17         # only these
    python3 scripts/mutate.py --gtest-src DIR # reuse an existing GoogleTest checkout (no network)

The mutants are applied in a copy of the repository, never in the working tree.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (id, file, original text, replacement, what it breaks)
MUTANTS: list[tuple[str, str, str, str, str]] = [
    ("M01", "src/block_manager.cpp", "  if (b.in_map) {\n    lru_push_back(id);", "  if (false) {\n    lru_push_back(id);",
     "freed blocks are never kept in the cache"),
    ("M02", "src/block_manager.cpp", "const BlockId id = lru_head_;", "const BlockId id = lru_tail_;",
     "the newest cached block is evicted first"),
    ("M03", "src/block_manager.cpp", "  by_hash_.erase(blocks_[id].hash);\n", "",
     "an evicted block stays in the hash map"),
    ("M04", "src/block_manager.cpp", "const std::size_t max_hits = (prompt.size() - 1) / bs;",
     "const std::size_t max_hits = prompt.size() / bs;", "the block with the last token can be reused"),
    ("M05", "src/block_manager.cpp", "if (b.parent_hash != parent || !std::ranges::equal(b.tokens, tokens)) break;",
     "if (!std::ranges::equal(b.tokens, tokens)) break;", "the parent hash is not checked on a hit"),
    ("M06", "src/block_manager.cpp", "if (b.parent_hash != parent || !std::ranges::equal(b.tokens, tokens)) break;",
     "if (b.parent_hash != parent) break;", "the tokens are not checked on a hit"),
    ("M07", "src/block_manager.cpp", "num_available_blocks() - hits_in_lru;", "num_available_blocks();",
     "a reused cached block also counts as room for new ones"),
    ("M08", "src/block_manager.cpp", "      if (b.ref == 0) ++hits_in_lru;\n", "",
     "hits waiting in the LRU list are not counted"),
    ("M09", "src/block_manager.cpp", "  if (b.in_lru) lru_remove(id);\n  ++b.ref;", "  ++b.ref;",
     "a reused block stays on the eviction list"),
    ("M10", "src/block_manager.cpp", "stats_.hit_blocks += p.hits.size();", "stats_.hit_blocks += 0;",
     "hits are not counted"),
    ("M11", "src/block_manager.cpp", "const bool cow = room > 0 && blocks_[seq.table.back()].ref > 1;",
     "const bool cow = false;", "a shared partial block is written in place"),
    ("M12", "src/block_manager.cpp", "    --blocks_[shared].ref;  // it was shared, so it is still in use\n", "",
     "the copied-from block keeps a reference that is gone"),
    ("M13", "src/block_manager.cpp", "(extra + bs - 1) / bs + (cow ? 1 : 0);", "(extra + bs - 1) / bs;",
     "a copy is not counted when checking for room"),
    ("M14", "src/block_manager.cpp", "if (last.tokens.size() == bs) {", "if (last.tokens.size() > bs) {",
     "generated blocks are never sealed"),
    ("M15", "src/block_manager.cpp", "      seq.last_full_hash = last.hash;\n", "",
     "the hash chain is lost after a generated block"),
    ("M16", "src/block_manager.cpp", "for (const BlockId b : copy.table) ++blocks_[b].ref;", "",
     "a forked sequence does not hold references"),
    ("M17", "src/block_manager.cpp", "std::views::reverse(table)", "table",
     "blocks are freed head first"),
    ("M18", "src/block_manager.cpp", "b.in_map = by_hash_.try_emplace(b.hash, id).second;",
     "by_hash_[b.hash] = id;\n  b.in_map = true;", "a duplicate block takes the hash from the first holder"),
    ("M19", "src/block_manager.cpp", "  if (!cfg_.enable_prefix_cache) return;  // nothing will ever look the block up: skip the hashing\n", "",
     "blocks are registered even with the cache off"),
    ("M20", "src/block_manager.cpp", "return free_.size() + lru_size_;", "return free_.size();",
     "cached blocks are not available for reuse"),
    ("M21", "src/block_manager.cpp", "    reset(b);\n    free_.push_back(id);", "    free_.push_back(id);",
     "a freed block keeps its old content"),
    ("M22", "src/block_manager.cpp", "    parent = blocks_[b].hash;\n  }\n  for (std::size_t pos", "  }\n  for (std::size_t pos",
     "new blocks after a hit are chained to the wrong parent"),
    ("M23", "src/block_manager.cpp", "  if (seqs_.contains(id)) return {.status = Status::DuplicateSequence};\n", "",
     "an id can be allocated twice"),
    ("M24", "src/block_manager.cpp", "  if (seqs_.contains(child)) return Status::DuplicateSequence;\n", "",
     "a fork can overwrite a live sequence"),
    ("M25", "src/block_manager.cpp", "s.used_blocks = cfg_.num_blocks - free_.size() - lru_size_;",
     "s.used_blocks = cfg_.num_blocks - free_.size();", "cached blocks are counted as in use"),
    ("M26", "src/block_manager.cpp", "    ++stats_.failed_requests;\n    return {.status = Status::OutOfBlocks};\n  }\n  const std::size_t bs",
     "    return {.status = Status::OutOfBlocks};\n  }\n  const std::size_t bs", "a refused allocation is not counted"),
    ("M27", "src/block_manager.cpp", "    ++stats_.cow_copies;\n", "", "copies are not counted"),
    ("M28", "src/block_manager.cpp", "  ++stats_.evictions;\n", "", "evictions are not counted"),
    ("M29", "src/block_manager.cpp", "stats_.lookup_blocks += p.full_blocks;", "stats_.lookup_blocks += p.hits.size();",
     "the hit rate is always one"),
    ("M30", "include/kvbm/hash.hpp", "return mix64(parent ^ hash_tokens(tokens));", "return mix64(hash_tokens(tokens));",
     "the hash ignores the prefix"),
    ("M31", "include/kvbm/hash.hpp", "for (const TokenId t : tokens) {\n    h = mix64",
     "for (const TokenId t : tokens.first(tokens.empty() ? 0 : tokens.size() - 1)) {\n    h = mix64",
     "the hash ignores the last token of a block"),
]


def run(cmd: list[str], cwd: Path, timeout: float) -> tuple[int, str]:
    try:
        out = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "timeout"
    return out.returncode, out.stdout + out.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ids", nargs="*", help="run only these mutant ids")
    parser.add_argument("--gtest-src", help="an existing GoogleTest source directory, to avoid the download")
    parser.add_argument("--timeout", type=float, default=300.0, help="per build or test run, in seconds")
    args = parser.parse_args()

    mutants = [m for m in MUTANTS if not args.ids or m[0] in args.ids]
    bad = 0
    with tempfile.TemporaryDirectory(prefix="kvbm-mutants-") as tmp:
        work = Path(tmp) / "repo"
        shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".git", "build*", "*.o", ".cache"))
        configure = ["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug", "-DKVBM_BUILD_BENCH=OFF"]
        if args.gtest_src:
            configure.append(f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={Path(args.gtest_src).resolve()}")
        code, log = run(configure, work, args.timeout)
        if code:
            print(log[-2000:])
            return 2
        build = ["cmake", "--build", "build", "-j", "8"]
        test = ["./build/kvbm_tests", "--gtest_fail_fast", "--gtest_brief=1"]
        code, log = run(build, work, args.timeout)
        if code:
            print(log[-2000:])
            return 2
        code, log = run(test, work, args.timeout)
        if code:
            print("the unmutated tests do not pass:\n" + log[-2000:])
            return 2

        for mid, name, old, new, what in mutants:
            path = work / name
            original = path.read_text()
            if original.count(old) != 1:
                print(f"{mid}: anchor found {original.count(old)} times in {name}: {old!r}")
                bad += 1
                continue
            path.write_text(original.replace(old, new))
            started = time.monotonic()
            try:
                code, log = run(build, work, args.timeout)
                if code:
                    errors = [ln for ln in log.splitlines() if "error:" in ln]
                    # a static_assert in the tests failing is the tests noticing
                    verdict = "killed" if "static assertion failed" in log else "build"
                    why = errors[0][-110:] if errors else "does not compile"
                else:
                    code, log = run(test, work, args.timeout)
                    failed = [ln for ln in log.splitlines() if ln.startswith("[  FAILED  ]")]
                    verdict = {0: "SURVIVED", 124: "hang"}.get(code, "killed")
                    why = failed[0][13:90] if failed else ""
            finally:
                path.write_text(original)
            took = time.monotonic() - started
            print(f"{mid} {verdict:8s} {took:5.1f}s  {what}" + (f"  [{why}]" if why else ""), flush=True)
            bad += verdict in ("SURVIVED", "build")
    print(f"\n{len(mutants) - bad} of {len(mutants)} mutants killed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
