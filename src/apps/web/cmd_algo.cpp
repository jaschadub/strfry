#include <docopt.h>

#include "golpe.h"

#include "AlgoParser.h"


static const char USAGE[] =
R"(
    Usage:
      algo
)";


void cmd_algo(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    auto txn = env.txn_ro();

    std::string str;

    {
        std::string line;
        while (std::getline(std::cin, line)) {
            str += line;
            str += "\n";
        }
    }

    auto alg = parseAlgo(txn, str);

    for (const auto &[k, v] : alg.variableIndexLookup) {
        LI << k << " = " << alg.pubkeySets[v].size() << " recs";
    }
}
