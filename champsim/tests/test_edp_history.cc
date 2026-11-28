// The EDP trainer skips zero entries when it reads up to four older requests.
#include "../modules/prefetcher/edp/edp.cc"

#include <iostream>

const std::size_t NUM_CPUS = 1;
const unsigned BLOCK_SIZE = 64, PAGE_SIZE = 4096, LOG2_BLOCK_SIZE = 6,
               LOG2_PAGE_SIZE = 12;

int
main()
{
    constexpr uint64_t current_ip = 19;
    using triggers = std::array<uint64_t, numFindIPTriggers>;
    if (train_found_trigger_IP(10, current_ip, 0) !=
        triggers{current_ip, 0, 0, 0}) {
        std::cerr << "Empty history must use only the current instruction\n";
        return 1;
    }
    auto &history = instructionHistory[0].entries;
    history.emplace_back(11, 7);
    history.emplace_back(12, 9);
    history.emplace_back(13, 15);
    if (train_found_trigger_IP(10, current_ip, 0) != triggers{12, 11, 0, 0}) {
        std::cerr
            << "Only valid earlier instructions may trigger a prefetch\n";
        return 1;
    }
    std::cout << "ok empty and partial data history\n";
}
