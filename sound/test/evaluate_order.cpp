// evaluate_order.cpp
// ==================
// Compares an original (correct-order) MP3 against your algorithm's output
// and prints a per-metric score - no composite grade, just the numbers.
//
// Chunks are MP3 frames read via the real Mp3FrameScanner (frame_scanner.hpp),
// using Mp3FrameScanner::getRawChunks() - each frame's raw bytes is one chunk.
//
// Usage:
//     evaluate_order original.mp3 algorithm_output.mp3

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include "../frame_scanner.hpp"

// ══════════════════════════════════════════════════════════════════════════
// ── MP3 INTERFACE ────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

/** Reads MP3 frames using Mp3FrameScanner. Each frame's raw bytes becomes one chunk. */
static std::vector<std::string> read_chunks(const std::string& path) {
    Mp3FrameScanner scanner(path);
    std::vector<std::string> chunks;
    for (const std::vector<uint8_t>& raw : scanner.getRawChunks()) {
        chunks.emplace_back(raw.begin(), raw.end());
    }
    return chunks;
}

// ══════════════════════════════════════════════════════════════════════════
// ── MATCHING ─────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

struct ChunkMatch {
    int orig_index;                 // position in original  (0-based)
    std::optional<int> algo_index;  // position in algorithm output, nullopt if missing
};

/** For each original chunk, find where it ended up in the algo output. */
static std::vector<ChunkMatch> match_chunks(const std::vector<std::string>& orig_chunks,
                                             const std::vector<std::string>& algo_chunks) {
    std::unordered_map<std::string, std::vector<int>> algo_positions;
    for (int i = 0; i < static_cast<int>(algo_chunks.size()); ++i) {
        algo_positions[algo_chunks[i]].push_back(i);
    }

    std::vector<bool> used(algo_chunks.size(), false);
    std::vector<ChunkMatch> matches;
    matches.reserve(orig_chunks.size());

    for (int orig_idx = 0; orig_idx < static_cast<int>(orig_chunks.size()); ++orig_idx) {
        std::vector<int> candidates;
        auto it = algo_positions.find(orig_chunks[orig_idx]);
        if (it != algo_positions.end()) {
            for (int p : it->second) if (!used[p]) candidates.push_back(p);
        }

        if (!candidates.empty()) {
            int best = *std::min_element(candidates.begin(), candidates.end(),
                [orig_idx](int a, int b) { return std::abs(a - orig_idx) < std::abs(b - orig_idx); });
            used[best] = true;
            matches.push_back({orig_idx, best});
        } else {
            matches.push_back({orig_idx, std::nullopt});
        }
    }

    return matches;
}

// ══════════════════════════════════════════════════════════════════════════
// ── METRICS ──────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

struct PresenceResult { int found; int total; double pct; };

static PresenceResult metric_presence(const std::vector<ChunkMatch>& matches) {
    int found = 0;
    for (const ChunkMatch& m : matches) if (m.algo_index) ++found;
    return {found, static_cast<int>(matches.size()), matches.empty() ? 0.0 :
            static_cast<double>(found) / static_cast<double>(matches.size()) * 100.0};
}

struct ExactPositionResult { int exact; int total; double pct; };

static ExactPositionResult metric_exact_position(const std::vector<ChunkMatch>& matches) {
    int exact = 0;
    for (const ChunkMatch& m : matches) if (m.algo_index && *m.algo_index == m.orig_index) ++exact;
    return {exact, static_cast<int>(matches.size()), matches.empty() ? 0.0 :
            static_cast<double>(exact) / static_cast<double>(matches.size()) * 100.0};
}

/** Kendall's tau: +1 = perfect order, 0 = random, -1 = fully reversed.
 *  Only counts chunks that were found in the output. */
static std::optional<double> metric_kendall_tau(const std::vector<ChunkMatch>& matches) {
    std::vector<int> positions;
    for (const ChunkMatch& m : matches) if (m.algo_index) positions.push_back(*m.algo_index);
    if (positions.size() < 2) return std::nullopt;

    const int n = static_cast<int>(positions.size());
    long long concordant = 0, discordant = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (positions[i] < positions[j]) ++concordant;
            else ++discordant;
        }
    }
    const long long total = concordant + discordant;
    return total ? static_cast<double>(concordant - discordant) / static_cast<double>(total) : 0.0;
}

struct CorrectSequencesResult { std::vector<int> runs; int covered; double pct; };

/** Finds all runs where consecutive original chunks are also consecutive
 *  in the algo output (correct relative order AND adjacency). */
static CorrectSequencesResult metric_correct_sequences(const std::vector<ChunkMatch>& matches, int min_run = 2) {
    std::vector<ChunkMatch> found;
    for (const ChunkMatch& m : matches) if (m.algo_index) found.push_back(m);
    std::sort(found.begin(), found.end(), [](const ChunkMatch& a, const ChunkMatch& b) {
        return a.orig_index < b.orig_index;
    });

    std::vector<int> runs;
    int current = 1;
    for (std::size_t i = 1; i < found.size(); ++i) {
        const ChunkMatch& prev = found[i - 1];
        const ChunkMatch& cur = found[i];
        if (cur.orig_index == prev.orig_index + 1 && *cur.algo_index == *prev.algo_index + 1) {
            ++current;
        } else {
            if (current >= min_run) runs.push_back(current);
            current = 1;
        }
    }
    if (!found.empty() && current >= min_run) runs.push_back(current);

    int covered = 0;
    for (int r : runs) covered += r;
    double pct = matches.empty() ? 0.0 : static_cast<double>(covered) / static_cast<double>(matches.size()) * 100.0;
    return {runs, covered, pct};
}

struct DisplacementResult { std::optional<double> avg; std::optional<double> avg_norm; };

/** Average number of chunk positions each chunk drifted from its correct slot,
 *  also normalised to [0, 1] relative to file length. */
static DisplacementResult metric_avg_displacement(const std::vector<ChunkMatch>& matches) {
    std::vector<int> displacements;
    for (const ChunkMatch& m : matches) if (m.algo_index) displacements.push_back(std::abs(*m.algo_index - m.orig_index));
    if (displacements.empty()) return {std::nullopt, std::nullopt};

    const int n = static_cast<int>(matches.size());
    double sum = 0;
    for (int d : displacements) sum += d;
    const double avg = sum / static_cast<double>(displacements.size());
    const double avg_norm = avg / static_cast<double>(std::max(n - 1, 1));
    return {avg, avg_norm};
}

// ══════════════════════════════════════════════════════════════════════════
// ── REPORT ───────────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

static const int W = 60;

static std::string repeat(char c, int n) { return std::string(std::max(n, 0), c); }

static std::string centered(const std::string& text, int width) {
    if (static_cast<int>(text.size()) >= width) return text;
    const int total_pad = width - static_cast<int>(text.size());
    const int left = total_pad / 2;
    return std::string(left, ' ') + text + std::string(total_pad - left, ' ');
}

static void print_report(const std::string& orig_path, const std::string& algo_path,
                          const std::vector<ChunkMatch>& matches) {
    const PresenceResult presence           = metric_presence(matches);
    const ExactPositionResult exact         = metric_exact_position(matches);
    const std::optional<double> tau         = metric_kendall_tau(matches);
    const CorrectSequencesResult sequences  = metric_correct_sequences(matches);
    const DisplacementResult displacement   = metric_avg_displacement(matches);

    std::cout << repeat('=', W) << "\n";
    std::cout << centered("MP3 CHUNK ORDER EVALUATION", W) << "\n";
    std::cout << repeat('=', W) << "\n";
    std::cout << "  Original : " << orig_path << "\n";
    std::cout << "  Algorithm: " << algo_path << "\n";
    std::cout << repeat('-', W) << "\n";

    std::cout << "\n  Presence\n";
    std::cout << "    " << presence.found << "/" << presence.total << " chunks found in output\n";
    std::cout << "    -> " << std::fixed << std::setprecision(1) << presence.pct << "%\n";

    std::cout << "\n  Exact position\n";
    std::cout << "    " << exact.exact << "/" << exact.total << " chunks in the correct slot\n";
    std::cout << "    -> " << std::fixed << std::setprecision(1) << exact.pct << "%\n";

    std::cout << "\n  Relative order  (Kendall's tau)\n";
    if (!tau) {
        std::cout << "    -> not enough matched chunks\n";
    } else {
        std::cout << "    tau ranges from -1 (fully reversed) to +1 (perfect order)\n";
        std::cout << "    -> tau = " << std::showpos << std::fixed << std::setprecision(4) << *tau
                   << std::noshowpos << "\n";
    }

    std::cout << "\n  Correct adjacent sequences  (runs of >=2 consecutive chunks)\n";
    if (sequences.runs.empty()) {
        std::cout << "    No correct runs found\n";
    } else {
        std::cout << "    Run lengths: [";
        for (std::size_t i = 0; i < sequences.runs.size(); ++i) {
            std::cout << sequences.runs[i];
            if (i + 1 < sequences.runs.size()) std::cout << ", ";
        }
        std::cout << "]\n";
        std::cout << "    " << sequences.covered << "/" << presence.total << " chunks covered by correct runs\n";
    }
    std::cout << "    -> " << std::fixed << std::setprecision(1) << sequences.pct << "%\n";

    std::cout << "\n  Average displacement per chunk\n";
    if (!displacement.avg) {
        std::cout << "    -> no matched chunks\n";
    } else {
        std::cout << "    Average: " << std::fixed << std::setprecision(2) << *displacement.avg
                   << " chunk positions off\n";
        std::cout << "    -> " << std::fixed << std::setprecision(1) << (*displacement.avg_norm * 100.0)
                   << "% of file length\n";
    }

    std::cout << "\n" << repeat('=', W) << "\n";

    std::cout << "\n  CHUNK MAP  [original -> algo position]  (? = missing)\n\n";
    std::vector<ChunkMatch> ordered = matches;
    std::sort(ordered.begin(), ordered.end(), [](const ChunkMatch& a, const ChunkMatch& b) {
        return a.orig_index < b.orig_index;
    });

    int col = 0;
    for (const ChunkMatch& m : ordered) {
        std::string a = m.algo_index ? std::to_string(*m.algo_index) : "?";
        std::string cell = "[" + std::to_string(m.orig_index) + "->" + a + "]";
        if (col + static_cast<int>(cell.size()) + 1 > W - 2) {
            std::cout << "\n";
            col = 0;
        }
        std::cout << cell << " ";
        col += static_cast<int>(cell.size()) + 1;
    }
    std::cout << "\n\n";
}

// ══════════════════════════════════════════════════════════════════════════
// ── ENTRY POINT ──────────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: evaluate_order <original.mp3> <algorithm_output.mp3>\n";
        return 1;
    }

    const std::string original = argv[1];
    const std::string algorithm = argv[2];

    const std::vector<std::string> orig_chunks = read_chunks(original);
    const std::vector<std::string> algo_chunks = read_chunks(algorithm);
    const std::vector<ChunkMatch> matches = match_chunks(orig_chunks, algo_chunks);

    print_report(original, algorithm, matches);

    return 0;
}
