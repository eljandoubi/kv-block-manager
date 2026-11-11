// A small serving simulation: what does prefix caching buy when many requests share a system prompt?
//
//   kvbm_simulate [--blocks N] [--block-size N] [--requests N] [--templates N] [--zipf S] [--seed N]
//
// A queue of requests is served as fast as the pool of KV blocks allows. Each decode step every
// running sequence generates one token. When there is no block for it, the newest sequence is
// preempted and put back at the front of the queue (it will compute its prompt again). The same
// workload runs twice, with the prefix cache off and on. Time is counted in decode steps, and the
// prompt tokens that had to be computed are counted separately, because that is the work the cache
// saves.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "kvbm/block_manager.hpp"

namespace {

using kvbm::AllocResult;
using kvbm::BlockManager;
using kvbm::Config;
using kvbm::SeqId;
using kvbm::Status;
using kvbm::TokenId;

struct Options {
  std::size_t blocks = 4096;
  std::size_t block_size = 16;
  std::size_t requests = 2000;
  std::size_t templates = 8;
  double zipf = 1.0;
  std::uint64_t seed = 1;
};

struct Request {
  SeqId id;
  std::vector<TokenId> prompt;
  std::size_t to_generate;
  std::size_t generated = 0;
};

struct Outcome {
  std::size_t steps = 0;
  double mean_running = 0;
  double hit_rate = 0;
  std::uint64_t prompt_tokens_computed = 0;
  std::uint64_t prompt_tokens_cached = 0;
  std::uint64_t preemptions = 0;
  std::uint64_t evictions = 0;
  std::size_t peak_used_blocks = 0;
};

std::vector<Request> make_workload(const Options& o) {
  std::mt19937_64 rng(o.seed);
  std::vector<std::vector<TokenId>> templates;
  const std::array<std::size_t, 4> lengths = {256, 512, 1024, 2048};
  for (std::size_t t = 0; t < o.templates; ++t) {
    std::vector<TokenId> system(lengths[t % 4]);
    for (std::size_t i = 0; i < system.size(); ++i) system[i] = static_cast<TokenId>(1000 * (t + 1) + i);
    templates.push_back(std::move(system));
  }
  std::vector<double> weights(o.templates);
  for (std::size_t t = 0; t < o.templates; ++t) weights[t] = 1.0 / std::pow(static_cast<double>(t + 1), o.zipf);
  std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());

  std::vector<Request> out;
  for (std::size_t i = 0; i < o.requests; ++i) {
    Request r;
    r.id = i + 1;
    r.prompt = templates[pick(rng)];
    const std::size_t user = 32 + rng() % 225;  // 32..256 tokens of their own
    for (std::size_t k = 0; k < user; ++k) r.prompt.push_back(static_cast<TokenId>(100000 + rng() % 100000));
    r.to_generate = 64 + rng() % 193;  // 64..256 tokens
    out.push_back(std::move(r));
  }
  return out;
}

Outcome simulate(const Options& o, bool cache, std::vector<Request> workload) {
  BlockManager m(Config{.num_blocks = o.blocks, .block_size = o.block_size, .enable_prefix_cache = cache});
  std::deque<Request> waiting(std::make_move_iterator(workload.begin()), std::make_move_iterator(workload.end()));
  std::vector<Request> running;
  Outcome out;
  std::mt19937_64 rng(o.seed + 1);
  std::uint64_t running_sum = 0;
  std::size_t done = 0;
  const std::size_t total = waiting.size();

  while (done < total) {
    // admit as many as fit, in order
    while (!waiting.empty()) {
      Request& next = waiting.front();
      if (!m.can_allocate(next.prompt)) break;
      const AllocResult r = m.allocate(next.id, next.prompt);
      out.prompt_tokens_cached += r.cached_tokens;
      out.prompt_tokens_computed += next.prompt.size() - r.cached_tokens;
      running.push_back(std::move(next));
      waiting.pop_front();
    }
    if (running.empty()) {
      std::fprintf(stderr, "the pool is too small for a single prompt\n");
      std::exit(EXIT_FAILURE);
    }
    // one decode step
    for (std::size_t i = 0; i < running.size();) {
      Request& r = running[i];
      const auto token = static_cast<TokenId>(rng() % 50000);
      if (m.append(r.id, std::span<const TokenId>(&token, 1)).status == Status::OutOfBlocks) {
        // preempt the newest sequence (the last one running) and try this one again next step
        Request victim = std::move(running.back());
        running.pop_back();
        m.free(victim.id);
        victim.generated = 0;
        waiting.push_front(std::move(victim));
        ++out.preemptions;
        if (i >= running.size()) break;
        continue;
      }
      ++r.generated;
      if (r.generated >= r.to_generate) {
        m.free(r.id);
        running[i] = std::move(running.back());
        running.pop_back();
        ++done;
        continue;
      }
      ++i;
    }
    ++out.steps;
    running_sum += running.size();
    out.peak_used_blocks = std::max(out.peak_used_blocks, m.stats().used_blocks);
  }
  const auto s = m.stats();
  out.mean_running = static_cast<double>(running_sum) / static_cast<double>(out.steps);
  out.hit_rate = s.hit_rate();
  out.evictions = s.evictions;
  return out;
}

bool arg_value(int argc, char** argv, int& i, const char* name, std::string& value) {
  if (std::strcmp(argv[i], name) != 0) return false;
  if (i + 1 >= argc) {
    std::fprintf(stderr, "%s needs a value\n", name);
    std::exit(2);
  }
  value = argv[++i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): bad numbers may end the program
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    if (arg_value(argc, argv, i, "--blocks", v))
      o.blocks = std::stoul(v);
    else if (arg_value(argc, argv, i, "--block-size", v))
      o.block_size = std::stoul(v);
    else if (arg_value(argc, argv, i, "--requests", v))
      o.requests = std::stoul(v);
    else if (arg_value(argc, argv, i, "--templates", v))
      o.templates = std::stoul(v);
    else if (arg_value(argc, argv, i, "--zipf", v))
      o.zipf = std::stod(v);
    else if (arg_value(argc, argv, i, "--seed", v))
      o.seed = std::stoull(v);
    else {
      std::fprintf(stderr,
                   "usage: %s [--blocks N] [--block-size N] [--requests N] [--templates N] [--zipf S] [--seed N]\n",
                   argv[0]);
      return 2;
    }
  }
  const auto workload = make_workload(o);
  const Outcome off = simulate(o, false, workload);
  const Outcome on = simulate(o, true, workload);

  std::printf(
      "%zu requests; %zu shared system prompts of 256-2048 tokens (zipf %.1f) + 32-256 own tokens; 64-256 generated\n",
      o.requests, o.templates, o.zipf);
  std::printf("pool: %zu blocks x %zu tokens = %zu tokens of KV cache\n\n", o.blocks, o.block_size,
              o.blocks * o.block_size);
  std::printf("%-34s %14s %14s\n", "", "cache off", "cache on");
  std::printf("%-34s %14zu %14zu\n", "decode steps to finish", off.steps, on.steps);
  std::printf("%-34s %14.1f %14.1f\n", "sequences running per step (mean)", off.mean_running, on.mean_running);
  std::printf("%-34s %14llu %14llu\n", "prompt tokens computed",
              static_cast<unsigned long long>(off.prompt_tokens_computed),
              static_cast<unsigned long long>(on.prompt_tokens_computed));
  std::printf("%-34s %14llu %14llu\n", "prompt tokens served from cache",
              static_cast<unsigned long long>(off.prompt_tokens_cached),
              static_cast<unsigned long long>(on.prompt_tokens_cached));
  std::printf("%-34s %14s %13.1f%%\n", "block hit rate", "-", 100.0 * on.hit_rate);
  std::printf("%-34s %14llu %14llu\n", "preemptions", static_cast<unsigned long long>(off.preemptions),
              static_cast<unsigned long long>(on.preemptions));
  std::printf("%-34s %14llu %14llu\n", "evictions", static_cast<unsigned long long>(off.evictions),
              static_cast<unsigned long long>(on.evictions));
  std::printf("%-34s %14zu %14zu\n", "peak blocks in use", off.peak_used_blocks, on.peak_used_blocks);
  return EXIT_SUCCESS;
}
