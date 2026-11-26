// Implementation of the knobbed branch predictor. See knobbed_bp.h for the knob
// list and README.md for how to configure it.
//
// ChampSim calls predict_branch(ip) for EVERY instruction (it does not know yet
// whether the instruction is a branch) and last_branch_result() only for real
// branches, right after the prediction. So:
//   predict_branch      pure: computes a prediction and records it in `last`
//   last_branch_result  trains the entries recorded in `last`, then pushes the
//                       outcome into the history registers
// To avoid running the (expensive) predictor on every non-branch instruction, a
// small "seen as a branch" filter short-circuits instructions that have never
// been a branch. The first dynamic occurrence of each static branch is therefore
// predicted not-taken; it is still trained.
#include "knobbed_bp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "instruction.h"

namespace kbp
{
namespace
{
std::string trim(std::string s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  const auto e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}
bool is_on(const std::string& v) { return v == "1" || v == "on" || v == "true"; }

constexpr int GSHARE_LADDER[LADDER_LEVELS] = {2, 4, 6, 8, 10, 12, 16, 24};
constexpr int PERC_LADDER[LADDER_LEVELS] = {4, 8, 12, 16, 24, 32, 48, 64};
constexpr int TAGE_LADDER[LADDER_LEVELS] = {8, 12, 16, 24, 32, 48, 64, 128};

// Unsigned saturating counter helpers (bimodal / gshare / TAGE base / selector).
int ctr_max(int bits) { return (1 << bits) - 1; }
bool ctr_taken(int v, int bits) { return v >= (1 << bits) / 2; }
int ctr_weak_not_taken(int bits) { return (1 << bits) / 2 - 1; }

uint64_t pc_hash(uint64_t ip) { return ip ^ (ip >> 12) ^ (ip >> 24); }
uint64_t mask(int w) { return (w >= 64) ? ~0ull : ((1ull << w) - 1); }
int eff_len(const hint& h, int len) { return std::min(len, h.hist == history_kind::LOCAL ? 64 : 256); }
} // namespace

void ctr_train(int8_t& v, bool t, int bits) { v = static_cast<int8_t>(t ? std::min<int>(v + 1, ctr_max(bits)) : std::max<int>(v - 1, 0)); }

int history_length(engine e, int level)
{
  const auto i = static_cast<std::size_t>(std::clamp(level, 0, LADDER_LEVELS - 1));
  switch (e) {
  case engine::GSHARE: return GSHARE_LADDER[i];
  case engine::PERCEPTRON: return PERC_LADDER[i];
  case engine::TAGE: return TAGE_LADDER[i];
  default: return 0;
  }
}

uint64_t bit_reg::extract(int pos, int n) const
{
  const int wi = pos >> 6, off = pos & 63;
  if (wi > 3)
    return 0;
  uint64_t v = w[wi] >> off;
  if (off + n > 64 && wi + 1 < 4)
    v |= w[wi + 1] << (64 - off);
  return v & mask(n);
}

uint64_t bit_reg::fold(int len, int width, bool do_fold) const
{
  len = std::min(len, 256);
  if (!do_fold)
    return extract(0, std::min(len, width));
  uint64_t v = 0;
  for (int pos = 0; pos < len; pos += width)
    v ^= extract(pos, std::min(width, len - pos));
  return v;
}

uint32_t hint::to_word() const
{
  uint32_t w = 0;
  w |= static_cast<uint32_t>(eng) & 0x3u;
  w |= (static_cast<uint32_t>(hist) & 0x3u) << 2;
  w |= (std::min<uint32_t>(level, LADDER_LEVELS - 1) & 0x7u) << 4; // [6:4]
  w |= ((std::clamp<uint32_t>(counter_bits, 1, 2) - 1) & 0x1u) << 7;
  w |= (loop ? 1u : 0u) << 8;
  w |= (hybrid ? 1u : 0u) << 9;
  w |= (fold ? 1u : 0u) << 10;
  return w;
}

hint hint::from_word(uint32_t w)
{
  hint h;
  h.eng = static_cast<engine>(w & 0x3u);
  h.hist = static_cast<history_kind>((w >> 2) & 0x3u);
  h.level = static_cast<uint8_t>((w >> 4) & 0x7u);
  h.counter_bits = static_cast<uint8_t>(((w >> 7) & 0x1u) + 1);
  h.loop = (w >> 8) & 1u;
  h.hybrid = (w >> 9) & 1u;
  h.fold = (w >> 10) & 1u;
  return h;
}

bool hint::apply(const std::string& spec, std::string* error)
{
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (item.empty())
      continue;
    const auto eq = item.find('=');
    if (eq == std::string::npos)
      return fail("expected key=value: " + item);
    const auto k = trim(item.substr(0, eq));
    const auto v = trim(item.substr(eq + 1));
    try {
      if (k == "engine") {
        if (v == "bimodal")
          eng = engine::BIMODAL;
        else if (v == "gshare")
          eng = engine::GSHARE;
        else if (v == "perceptron")
          eng = engine::PERCEPTRON;
        else if (v == "tage")
          eng = engine::TAGE;
        else
          return fail("engine must be bimodal|gshare|perceptron|tage");
      } else if (k == "history") {
        if (v == "global")
          hist = history_kind::GLOBAL;
        else if (v == "path")
          hist = history_kind::PATH;
        else if (v == "local")
          hist = history_kind::LOCAL;
        else if (v == "imli")
          hist = history_kind::IMLI;
        else
          return fail("history must be global|path|local|imli");
      } else if (k == "level") {
        const int n = std::stoi(v);
        if (n < 0 || n >= LADDER_LEVELS)
          return fail("level must be 0-7");
        level = static_cast<uint8_t>(n);
      } else if (k == "counter") {
        const int n = std::stoi(v);
        if (n < 1 || n > 2)
          return fail("counter must be 1 or 2");
        counter_bits = static_cast<uint8_t>(n);
      } else if (k == "loop")
        loop = is_on(v);
      else if (k == "hybrid")
        hybrid = is_on(v);
      else if (k == "fold")
        fold = is_on(v);
      else if (k == "word") {
        const auto w = static_cast<uint32_t>(std::stoul(v, nullptr, 0));
        if (!word_valid(w))
          return fail("word does not fit in 11 bits: " + v);
        *this = from_word(w);
      } else
        return fail("unknown knob: " + k);
    } catch (const std::exception&) {
      return fail("bad value for " + k + ": " + v);
    }
  }
  return true;
}

tables::tables(const hint& hh) : h(hh)
{
  const int cb = h.counter_bits;
  if (h.eng == engine::BIMODAL || h.eng == engine::GSHARE || h.eng == engine::TAGE)
    bim.assign(BIM_SIZE, static_cast<int8_t>(ctr_weak_not_taken(cb)));
  if (h.hybrid) {
    hbim.assign(BIM_SIZE, static_cast<int8_t>(ctr_weak_not_taken(cb)));
    sel.assign(BIM_SIZE, 2); // starts weakly on the engine
  }
  if (h.eng == engine::PERCEPTRON) {
    row_stride = 1 + PERC_MAXLEN + PERC_IMLI_W;
    perc.assign(PERC_ROWS * static_cast<std::size_t>(row_stride), 0);
  }
  if (h.eng == engine::TAGE) {
    const double hmax = std::max(4, eff_len(h, h.length()));
    for (int i = 0; i < TAGE_TABLES; ++i) {
      tage_len[i] = std::max(1, static_cast<int>(std::lround(2.0 * std::pow(hmax / 2.0, static_cast<double>(i) / (TAGE_TABLES - 1)))));
      tage[i].assign(TAGE_SIZE, tage_entry{});
    }
  }
  if (h.loop)
    loops.assign(LOOP_SIZE, loop_entry{});
}
} // namespace kbp

using namespace kbp; // brings in the helpers in kbp's anonymous namespace
using kbp::engine;
using kbp::hint;
using kbp::history_kind;

// ---------------------------------------------------------------------------
// Configuration (ChampSim modules have no per-run parameters, so: environment)
//   KNOBBED_BP_HINT="engine=tage,history=global,level=5,loop=1"   default hint
//   KNOBBED_BP_REGIONS=<file>   one region per line:  <start> <end> <key=value,...>
//       (byte addresses of the branch PC, '#' starts a comment). A region starts
//       from the default hint and overrides only the keys it lists.
// A malformed setting is fatal so a typo never silently runs the wrong experiment.
// ---------------------------------------------------------------------------
void knobbed_bp::load_environment()
{
  std::string err;
  if (const char* spec = std::getenv("KNOBBED_BP_HINT")) {
    hint h = regions[0].h;
    if (!h.apply(spec, &err)) {
      std::fprintf(stderr, "[knobbed_bp] KNOBBED_BP_HINT: %s\n", err.c_str());
      std::exit(1);
    }
    set_region_hint(0, h);
  }
  if (const char* path = std::getenv("KNOBBED_BP_REGIONS")) {
    std::ifstream in(path);
    if (!in) {
      std::fprintf(stderr, "[knobbed_bp] cannot open KNOBBED_BP_REGIONS file %s\n", path);
      std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
      line = line.substr(0, line.find('#'));
      std::stringstream ss(line);
      std::string s, e, rest;
      if (!(ss >> s >> e))
        continue;
      std::getline(ss, rest);
      hint h = regions[0].h;
      if (!h.apply(rest, &err)) {
        std::fprintf(stderr, "[knobbed_bp] %s: %s\n", path, err.c_str());
        std::exit(1);
      }
      add_region(std::stoull(s, nullptr, 0), std::stoull(e, nullptr, 0), h);
    }
  }
}

void knobbed_bp::initialize_branch_predictor()
{
  regions.clear();
  regions.emplace_back();
  regions[0].t = std::make_shared<kbp::tables>(regions[0].h);
  local_hist.assign(kbp::LOCAL_SIZE, 0);
  seen_branch.assign(SEEN_SIZE, 0);
  load_environment();
  const auto& h = regions[0].h;
  std::printf("[knobbed_bp] default hint: word=%#x engine=%d history=%d level=%d (length=%d) counter=%d loop=%d hybrid=%d fold=%d regions=%zu\n", h.to_word(),
              static_cast<int>(h.eng), static_cast<int>(h.hist), h.level, h.length(), h.counter_bits, h.loop, h.hybrid, h.fold, regions.size() - 1);
  std::printf("[knobbed_bp] fixed constants: bim=%zu perc_rows=%zu tage_tables=%d x %zu loop=%zu loop_conf=%d local_hist=%zu\n", kbp::BIM_SIZE, kbp::PERC_ROWS,
              kbp::TAGE_TABLES, kbp::TAGE_SIZE, kbp::LOOP_SIZE, kbp::LOOP_CONF, kbp::LOCAL_SIZE);
}

std::size_t knobbed_bp::add_region(uint64_t start, uint64_t end, const hint& h)
{
  kbp::region r;
  r.start = start;
  r.end = end;
  r.h = h;
  r.t = std::make_shared<kbp::tables>(h);
  regions.push_back(r);
  return regions.size() - 1;
}

void knobbed_bp::set_region_hint(std::size_t idx, const hint& h)
{
  if (idx < regions.size()) {
    regions[idx].h = h;
    regions[idx].t = std::make_shared<kbp::tables>(h); // retargeting starts with cold tables
  }
}

// Later regions win over earlier ones; index 0 is the catch-all default.
std::size_t knobbed_bp::find_region(uint64_t ip) const
{
  for (std::size_t i = regions.size(); i-- > 1;)
    if (ip >= regions[i].start && ip < regions[i].end)
      return i;
  return 0;
}

// The history register this hint reads. Local history is copied into scratch.
const kbp::bit_reg& knobbed_bp::view(const hint& h, uint64_t ip, kbp::bit_reg& scratch) const
{
  switch (h.hist) {
  case history_kind::PATH: return phr;
  case history_kind::LOCAL:
    scratch.w[0] = local_hist[pc_hash(ip) % kbp::LOCAL_SIZE];
    return scratch;
  default: return ghr; // GLOBAL and IMLI
  }
}

// Fold the newest `len` history bits into `width` bits. IMLI also mixes in the
// innermost-loop iteration count, so the same branch in iteration j of a loop gets
// its own context (the multidimensional-history idea).
uint64_t knobbed_bp::context(const hint& h, const kbp::bit_reg& v, int len, int width) const
{
  uint64_t c = len > 0 ? v.fold(eff_len(h, len), width, h.fold) : 0;
  if (h.hist == history_kind::IMLI)
    c ^= (static_cast<uint64_t>(imli) * 0x9E3779B1ull >> 5) & mask(width);
  return c;
}

kbp::pred_info knobbed_bp::compute(uint64_t ip) const
{
  kbp::pred_info p;
  p.valid = true;
  p.ip = ip;
  p.region = find_region(ip);
  const auto& reg = regions[p.region];
  const auto& h = reg.h;
  const auto& t = *reg.t;
  const int cb = h.counter_bits;
  const uint64_t pch = pc_hash(ip);
  kbp::bit_reg scratch;
  const auto& v = view(h, ip, scratch);
  const int len = h.length();

  p.bi = pch % kbp::BIM_SIZE;
  switch (h.eng) {
  case engine::BIMODAL: p.main_pred = ctr_taken(t.bim[p.bi], cb); break;
  case engine::GSHARE:
    p.bi = (pch ^ context(h, v, len, 12)) & (kbp::BIM_SIZE - 1);
    p.main_pred = ctr_taken(t.bim[p.bi], cb);
    break;
  case engine::PERCEPTRON: {
    p.perc_row = static_cast<int>(pch % kbp::PERC_ROWS);
    const int8_t* wts = &t.perc[static_cast<std::size_t>(p.perc_row) * static_cast<std::size_t>(t.row_stride)];
    const int n = std::min(eff_len(h, len), kbp::PERC_MAXLEN);
    int sum = wts[0];
    for (int i = 0; i < n; ++i)
      sum += v.bit(i) ? wts[1 + i] : -wts[1 + i];
    if (h.hist == history_kind::IMLI)
      sum += wts[1 + kbp::PERC_MAXLEN + (imli % kbp::PERC_IMLI_W)];
    p.perc_sum = sum;
    p.main_pred = sum >= 0;
    break;
  }
  case engine::TAGE: {
    for (int i = 0; i < kbp::TAGE_TABLES; ++i) {
      const int L = t.tage_len[i];
      p.tidx[i] = (pch ^ (pch >> 10) ^ context(h, v, L, 10)) & (kbp::TAGE_SIZE - 1);
      p.ttag[i] = static_cast<uint16_t>((pch ^ context(h, v, L, 8) ^ (context(h, v, L, 7) << 1)) & 0xFF);
    }
    for (int i = kbp::TAGE_TABLES - 1; i >= 0; --i) {
      const auto& e = t.tage[i][p.tidx[i]];
      if (e.valid && e.tag == p.ttag[i]) {
        if (p.provider < 0)
          p.provider = i;
        else if (p.alt < 0) {
          p.alt = i;
          break;
        }
      }
    }
    const bool base_pred = ctr_taken(t.bim[p.bi], cb);
    p.alt_pred = p.alt >= 0 ? t.tage[p.alt][p.tidx[p.alt]].ctr >= 0 : base_pred;
    if (p.provider >= 0) {
      const auto& e = t.tage[p.provider][p.tidx[p.provider]];
      p.provider_pred = e.ctr >= 0;
      const bool weak = (e.ctr == 0 || e.ctr == -1) && e.u == 0; // low confidence: trust the next longest
      p.main_pred = weak ? p.alt_pred : p.provider_pred;
    } else
      p.main_pred = base_pred;
    break;
  }
  }

  p.chosen = p.main_pred;
  if (h.hybrid) {
    p.hbi = pch % kbp::BIM_SIZE;
    p.bim_pred = ctr_taken(t.hbim[p.hbi], cb);
    if (t.sel[p.hbi] < 2) { // selector prefers the plain bimodal
      p.chosen = p.bim_pred;
      p.hybrid_used = true;
    }
  }
  p.final_pred = p.chosen;
  if (h.loop) {
    p.li = pch % kbp::LOOP_SIZE;
    const auto& e = t.loops[p.li];
    if (e.valid && e.tag == ((pch >> 6) & 0xFFFF)) {
      p.loop_hit = true;
      if (e.conf >= kbp::LOOP_CONF) {
        p.loop_used = true;
        p.loop_pred = e.cur < e.trip ? e.dir : !e.dir;
        p.final_pred = p.loop_pred;
      }
    }
  }
  return p;
}

bool knobbed_bp::predict_branch(champsim::address ip_addr)
{
  const uint64_t ip = ip_addr.to<uint64_t>();
  if (!seen_branch[pc_hash(ip) % SEEN_SIZE]) {
    last.valid = false; // not known to be a branch: skip the work
    return false;
  }
  last = compute(ip);
  return last.final_pred;
}

void knobbed_bp::last_branch_result(champsim::address ip_addr, champsim::address branch_target, bool taken, uint8_t branch_type)
{
  const uint64_t ip = ip_addr.to<uint64_t>();
  const uint64_t pch = pc_hash(ip);
  seen_branch[pch % SEEN_SIZE] = 1;

  if (branch_type == BRANCH_CONDITIONAL) {
    const auto p = (last.valid && last.ip == ip) ? last : compute(ip);
    auto& reg = regions[p.region];
    auto& t = *reg.t;
    const auto& h = reg.h;
    const int cb = h.counter_bits;
    const int len = h.length();
    kbp::bit_reg scratch;
    const auto& v = view(h, ip, scratch); // history is still the one the prediction used

    // ---- telemetry ----
    ++reg.total.branches;
    if (p.final_pred != taken)
      ++reg.total.mispredicts;
    if (p.loop_used) {
      ++reg.total.loop_overrides;
      reg.total.loop_correct += (p.loop_pred == taken);
    }
    if (p.hybrid_used) {
      ++reg.total.hybrid_alt;
      reg.total.hybrid_alt_correct += (p.bim_pred == taken);
    }

    // ---- train the engine ----
    switch (h.eng) {
    case engine::BIMODAL:
    case engine::GSHARE: kbp::ctr_train(t.bim[p.bi], taken, cb); break;
    case engine::PERCEPTRON: {
      const int n = std::min(eff_len(h, len), kbp::PERC_MAXLEN);
      const int theta = static_cast<int>(1.93 * n + 14);
      if (p.main_pred != taken || std::abs(p.perc_sum) <= theta) {
        int8_t* wts = &t.perc[static_cast<std::size_t>(p.perc_row) * static_cast<std::size_t>(t.row_stride)];
        auto bump = [](int8_t& w, bool up) { w = static_cast<int8_t>(std::clamp(w + (up ? 1 : -1), -128, 127)); };
        bump(wts[0], taken);
        for (int i = 0; i < n; ++i)
          bump(wts[1 + i], v.bit(i) == taken); // weight grows when this history bit agreed with the outcome
        if (h.hist == history_kind::IMLI)
          bump(wts[1 + kbp::PERC_MAXLEN + (imli % kbp::PERC_IMLI_W)], taken);
      }
      break;
    }
    case engine::TAGE: {
      const int tcb = cb + 1;
      const int cmax = (1 << (tcb - 1)) - 1, cmin = -(1 << (tcb - 1));
      if (p.provider >= 0) {
        auto& e = t.tage[p.provider][p.tidx[p.provider]];
        if (p.provider_pred != p.alt_pred) {
          if (p.provider_pred == taken)
            e.u = static_cast<uint8_t>(std::min(e.u + 1, 3));
          else if (e.u > 0)
            --e.u;
        }
        e.ctr = static_cast<int8_t>(std::clamp(e.ctr + (taken ? 1 : -1), cmin, cmax));
      } else
        kbp::ctr_train(t.bim[p.bi], taken, cb);
      // On a wrong prediction, allocate an entry with a longer history.
      if (p.main_pred != taken && p.provider < kbp::TAGE_TABLES - 1) {
        bool done = false;
        for (int j = p.provider + 1; j < kbp::TAGE_TABLES && !done; ++j) {
          auto& e = t.tage[j][p.tidx[j]];
          if (!e.valid || e.u == 0) {
            e.valid = true;
            e.tag = p.ttag[j];
            e.ctr = static_cast<int8_t>(taken ? 0 : -1); // weak toward the outcome
            e.u = 0;
            done = true;
          }
        }
        if (!done) // every candidate is useful: age them so a slot frees up
          for (int j = p.provider + 1; j < kbp::TAGE_TABLES; ++j)
            if (auto& e = t.tage[j][p.tidx[j]]; e.u > 0)
              --e.u;
      }
      if (++t.tick % (1u << 17) == 0) // periodic usefulness aging
        for (auto& tab : t.tage)
          for (auto& e : tab)
            e.u >>= 1;
      break;
    }
    }

    // ---- train the tournament ----
    if (h.hybrid) {
      if (p.main_pred != p.bim_pred) {
        auto& s = t.sel[p.hbi];
        s = static_cast<int8_t>(std::clamp<int>(s + (p.main_pred == taken ? 1 : -1), 0, 3));
      }
      kbp::ctr_train(t.hbim[p.hbi], taken, cb);
    }

    // ---- train the loop predictor ----
    if (h.loop) {
      auto& e = t.loops[p.li];
      const uint16_t tag = static_cast<uint16_t>((pch >> 6) & 0xFFFF);
      if (p.loop_hit) {
        if (taken == e.dir) {
          if (e.cur < 0xFFFF)
            ++e.cur;
          if (e.conf > 0 && e.cur > e.trip)
            e.conf = 0; // ran past the learned trip count: the pattern changed
        } else {
          if (e.cur == e.trip)
            e.conf = static_cast<uint8_t>(std::min(e.conf + 1, 3));
          else {
            e.trip = e.cur;
            e.conf = 0;
          }
          e.cur = 0;
        }
      } else if (p.final_pred != taken) {
        // Allocate on a misprediction. The rare outcome is probably the loop exit.
        if (e.valid && e.conf >= kbp::LOOP_CONF)
          --e.conf; // a confident entry is evicted gradually
        else {
          e = kbp::loop_entry{tag, 0, 0, 0, !taken, true};
        }
      }
    }
  }

  // ---- histories (all branch types feed the global and path registers) ----
  const bool is_cond = branch_type == BRANCH_CONDITIONAL;
  ghr.push(taken);
  phr.push(((ip >> 2) ^ (ip >> 4) ^ (ip >> 7) ^ (taken ? 1u : 0u)) & 1u);
  if (is_cond) {
    auto& lh = local_hist[pch % kbp::LOCAL_SIZE];
    lh = (lh << 1) | (taken ? 1u : 0u);
    if (branch_target.to<uint64_t>() <= ip) // backward branch: a loop edge
      imli = taken ? imli + 1 : 0;
  }
}

knobbed_bp::~knobbed_bp()
{
  // ChampSim has no end-of-run hook for branch predictors, so report from the destructor.
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const auto& r = regions[i];
    const auto& s = r.total;
    if (i == 0 && !s.branches)
      continue;
    std::printf("[knobbed_bp] (whole run incl. warmup) region %zu [%#llx,%#llx) word=%#x branches=%llu mispredicts=%llu rate=%.4f | loop_overrides=%llu loop_correct=%llu | "
                "hybrid_alt=%llu hybrid_alt_correct=%llu\n",
                i, static_cast<unsigned long long>(r.start), static_cast<unsigned long long>(r.end), r.h.to_word(), static_cast<unsigned long long>(s.branches),
                static_cast<unsigned long long>(s.mispredicts), s.rate(), static_cast<unsigned long long>(s.loop_overrides),
                static_cast<unsigned long long>(s.loop_correct), static_cast<unsigned long long>(s.hybrid_alt),
                static_cast<unsigned long long>(s.hybrid_alt_correct));
  }
}
