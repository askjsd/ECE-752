// Knobbed branch predictor: a ChampSim direction predictor whose behaviour is
// controlled by a "hint control word", the same idea as the knobbed prefetcher.
// The knobs come from the Instruction Flow lectures (Parts 3-5):
//
//   engine   -> how the prediction is made
//                 bimodal     PC-indexed saturating counters         (BHT, 1-bit / 2-bit)
//                 gshare      hash(PC, history) -> counters          (correlated two-level)
//                 perceptron  one weight per history bit, per PC row (branch weight table)
//                 tage        base + 4 partially-tagged tables with geometric history
//                             lengths, usefulness bits, alt-prediction on weak entries
//   history  -> what "how did we get here" means
//                 global      outcomes of the last N branches (BHR)
//                 path        the last N branch PCs (+ outcome) hashed into 1 bit each
//                 local       the last N outcomes of this branch only (local BHR table)
//                 imli        global history plus the innermost-loop iteration count
//   level    -> history length ladder 0-7 (the ladder depends on the engine)
//   counter  -> 1 or 2: width of the saturating counters (bimodal, gshare, TAGE base;
//               TAGE tagged counters are counter+1 bits wide). Perceptron ignores it.
//   fold     -> 1: history longer than the index is XOR-folded; 0: it is truncated
//   hybrid   -> 1: a tournament selector chooses between the engine and a plain bimodal
//   loop     -> 1: a loop-count predictor overrides the engine on confident loops
//
// Each region of the CODE address space (a range of branch PCs) can carry its own
// hint, mirroring the prefetcher's regions. Branches outside every region use the
// default hint. The *history registers are shared by all regions* (a branch's context
// does not depend on which region it lives in); the prediction tables are private to
// each region, so two regions never alias each other's counters.
//
// Not covered here (they are not part of ChampSim's direction-predictor interface):
// the BTB, the return address stack, and branch-resolution tricks (PUBS, decomposed
// branches, control-flow decoupling). The BTB is a separate module type.
#ifndef KNOBBED_BP_H
#define KNOBBED_BP_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "address.h"
#include "modules.h"

namespace kbp
{
enum class engine : uint8_t { BIMODAL = 0, GSHARE = 1, PERCEPTRON = 2, TAGE = 3 };
enum class history_kind : uint8_t { GLOBAL = 0, PATH = 1, LOCAL = 2, IMLI = 3 };

constexpr int LADDER_LEVELS = 8;
// History length in bits for (engine, level). Bimodal uses no history (returns 0).
// For TAGE this is the longest table's history; the other three are spread
// geometrically below it. LOCAL history holds at most 64 bits, so lengths are clamped.
int history_length(engine e, int level);

// The 11-bit hint. Every field uses all of its codes, so no word is an alias of another.
struct hint {
  engine eng = engine::GSHARE;
  history_kind hist = history_kind::GLOBAL;
  uint8_t level = 4;        // 0-7
  uint8_t counter_bits = 2; // 1 or 2
  bool loop = false;
  bool hybrid = false;
  bool fold = true;

  int length() const { return history_length(eng, level); }

  // Packed word:  [1:0] engine  [3:2] history  [6:4] level  [7] counter-1
  //               [8] loop      [9] hybrid     [10] fold
  uint32_t to_word() const;
  static bool word_valid(uint32_t word) { return word < (1u << 11); }
  static hint from_word(uint32_t word);
  // "key=value,key=value": engine history level counter loop hybrid fold word.
  bool apply(const std::string& spec, std::string* error);
};

// A bit register of the most recent outcomes; bit 0 is the newest.
struct bit_reg {
  std::array<uint64_t, 4> w{};
  void push(bool b)
  {
    for (int i = 3; i > 0; --i)
      w[i] = (w[i] << 1) | (w[i - 1] >> 63);
    w[0] = (w[0] << 1) | (b ? 1u : 0u);
  }
  bool bit(int i) const { return (w[i >> 6] >> (i & 63)) & 1u; }
  // n <= 32 bits starting at position pos
  uint64_t extract(int pos, int n) const;
  // XOR-fold the newest `len` bits into `width` bits (or just keep the newest `width` when !fold).
  uint64_t fold(int len, int width, bool do_fold) const;
};

struct stats {
  uint64_t branches = 0;      // conditional branches seen
  uint64_t mispredicts = 0;
  uint64_t loop_overrides = 0; // loop predictor supplied the final answer
  uint64_t loop_correct = 0;
  uint64_t hybrid_alt = 0;     // selector chose the plain bimodal over the engine
  uint64_t hybrid_alt_correct = 0;
  double rate() const { return branches ? static_cast<double>(mispredicts) / static_cast<double>(branches) : 0.0; }
};

struct tage_entry {
  uint16_t tag = 0;
  int8_t ctr = 0; // signed; taken if >= 0
  uint8_t u = 0;  // usefulness 0-3
  bool valid = false;
};

struct loop_entry {
  uint16_t tag = 0;
  uint16_t trip = 0; // dir outcomes before the exit
  uint16_t cur = 0;
  uint8_t conf = 0;  // 0-3; overrides at >= LOOP_CONF
  bool dir = true;
  bool valid = false;
};

constexpr std::size_t BIM_SIZE = 4096;
constexpr std::size_t PERC_ROWS = 512;
constexpr int PERC_MAXLEN = 64;
constexpr int PERC_IMLI_W = 64; // extra weights indexed by the loop iteration count (history=imli)
constexpr int TAGE_TABLES = 4;
constexpr std::size_t TAGE_SIZE = 1024;
constexpr std::size_t LOOP_SIZE = 64;
constexpr int LOOP_CONF = 2;
constexpr std::size_t LOCAL_SIZE = 1024;
void ctr_train(int8_t& v, bool taken, int bits); // unsigned saturating counter step

// Prediction tables for one region. Only what the hint needs is allocated.
struct tables {
  hint h;
  std::vector<int8_t> bim;     // bimodal engine / gshare / TAGE base
  std::vector<int8_t> hbim;    // plain bimodal used by the hybrid selector
  std::vector<int8_t> sel;     // tournament selector (2-bit, >=2 -> engine)
  std::vector<int8_t> perc;    // perceptron weights, PERC_ROWS x row_stride
  int row_stride = 0;
  std::array<std::vector<tage_entry>, TAGE_TABLES> tage;
  std::array<int, TAGE_TABLES> tage_len{};
  std::vector<loop_entry> loops;
  uint64_t tick = 0;           // branches trained, for TAGE usefulness aging
  explicit tables(const hint& hh);
};

struct region {
  uint64_t start = 0; // byte addresses of the branch PC, [start, end)
  uint64_t end = UINT64_MAX;
  hint h;
  std::shared_ptr<tables> t;
  stats total;
};

// What predict_branch() worked out, kept so last_branch_result() can train the
// same entries. predict_branch() is called for every instruction, so it must have
// no side effects; this record is the only thing it writes.
struct pred_info {
  bool valid = false;
  uint64_t ip = 0;
  std::size_t region = 0;
  bool main_pred = false, bim_pred = false, chosen = false, final_pred = false;
  bool hybrid_used = false;
  bool loop_hit = false, loop_used = false, loop_pred = false;
  std::size_t bi = 0, hbi = 0, li = 0;
  int perc_row = 0, perc_sum = 0;
  int provider = -1, alt = -1;
  bool provider_pred = false, alt_pred = false;
  std::array<std::size_t, TAGE_TABLES> tidx{};
  std::array<uint16_t, TAGE_TABLES> ttag{};
};
} // namespace kbp

struct knobbed_bp : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  ~knobbed_bp(); // prints per-region statistics (ChampSim has no final-stats hook for branch predictors)

  void initialize_branch_predictor();
  bool predict_branch(champsim::address ip);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type);

  // Runtime interface (for a driver): add a PC region or retarget one.
  std::size_t add_region(uint64_t start, uint64_t end, const kbp::hint& h);
  void set_region_hint(std::size_t idx, const kbp::hint& h);

private:
  std::vector<kbp::region> regions; // regions[0] is the default hint
  kbp::bit_reg ghr, phr;            // shared global and path history
  std::vector<uint64_t> local_hist; // shared local histories (up to 64 bits each)
  uint32_t imli = 0;                // innermost-loop iteration counter
  kbp::pred_info last;
  static constexpr std::size_t SEEN_SIZE = 1 << 20;
  std::vector<uint8_t> seen_branch; // 1 if an instruction with this (hashed) ip has ever been a branch

  void load_environment();
  std::size_t find_region(uint64_t ip) const;
  kbp::pred_info compute(uint64_t ip) const;
  const kbp::bit_reg& view(const kbp::hint& h, uint64_t ip, kbp::bit_reg& scratch) const;
  uint64_t context(const kbp::hint& h, const kbp::bit_reg& v, int len, int width) const;
};

#endif
