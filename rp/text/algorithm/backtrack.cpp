#include "backtrack.hpp"
#include <iostream>
#include <algorithm>

/** Quotes a string the way Python's repr() would show it in the progress log. */
static std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\') out.push_back('\\');
        if (c == '\n') { out += "\\n"; continue; }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static bool all_used(const std::vector<bool>& used) {
    return std::all_of(used.begin(), used.end(), [](bool b) { return b; });
}

std::vector<std::string> Backtrack::reconstruct_all(const std::string& output_file) {
    const std::size_t l = all_chunks.size();
    std::vector<std::string> results;
    std::vector<bool> used(l, false);

    std::ofstream f(output_file, std::ios::app);
    for (std::size_t i = 0; i < l; ++i) {
        std::cout << "Trying starting chunk " << (i + 1) << "/" << l << ": "
                  << quoted(all_chunks[i]) << "\n";
        used[i] = true;
        backtrack(all_chunks[i], used, f, results, /*find_all=*/true);
        used[i] = false;
    }

    return results;
}

std::optional<std::string> Backtrack::reconstruct_one(const std::string& output_file) {
    std::vector<bool> used(all_chunks.size(), false);
    std::vector<std::string> results;

    std::ofstream f(output_file, std::ios::app);
    for (std::size_t i = 0; i < all_chunks.size(); ++i) {
        used[i] = true;
        std::cout << "Trying starting chunk " << (i + 1) << "/" << all_chunks.size() << ": "
                  << quoted(all_chunks[i]) << "\n";
        std::optional<std::string> result = backtrack(all_chunks[i], used, f, results, /*find_all=*/false);
        used[i] = false;
        if (result) return result;
    }

    return std::nullopt;
}

std::optional<std::string> Backtrack::backtrack(const std::string& current_text,
                                                  std::vector<bool>& used,
                                                  std::ofstream& file,
                                                  std::vector<std::string>& results,
                                                  bool find_all) {
    if (all_used(used)) {
        if (validator.validate_text(current_text, all_chunks)) {
            results.push_back(current_text);
            file << current_text << "\n";
            return find_all ? std::nullopt : std::optional<std::string>(current_text);
        }
        return std::nullopt;
    }

    for (std::size_t i = 0; i < all_chunks.size(); ++i) {
        if (used[i]) continue;
        const std::string& ch = all_chunks[i];
        if (validator.validate_chunk(current_text, ch)) {
            used[i] = true;
            std::optional<std::string> found = backtrack(current_text + ch, used, file, results, find_all);
            used[i] = false;
            if (found && !find_all) return found;
        }
    }

    return std::nullopt;
}
