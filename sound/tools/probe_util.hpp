#ifndef RP_PROBE_UTIL_HPP
#define RP_PROBE_UTIL_HPP

// Helpers shared by the measurement probes in test/.

#include <algorithm>
#include <vector>

// Mann-Whitney AUC: the probability that a `wrong` value is larger than a `trueD` value, ties
// counted as 1/2. 0.5 = no signal, 1 = every true value below every wrong one. 0.5 if either is empty.
inline double auc(const std::vector<double>& trueD, std::vector<double> wrong) {
    if (trueD.empty() || wrong.empty()) return 0.5;
    std::sort(wrong.begin(), wrong.end());
    const double n = double(wrong.size());
    double acc = 0.0;
    for (double t : trueD) {
        const auto lo = std::lower_bound(wrong.begin(), wrong.end(), t);
        const auto hi = std::upper_bound(wrong.begin(), wrong.end(), t);
        const double less = double(lo - wrong.begin());   // wrong strictly less than t
        const double eq   = double(hi - lo);
        acc += ((n - (less + eq)) + 0.5 * eq) / n;         // P(wrong > t) + 0.5 P(==)
    }
    return acc / double(trueD.size());
}

#endif
