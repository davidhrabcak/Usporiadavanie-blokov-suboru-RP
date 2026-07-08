#include "two_sequence_dictionary_creator.hpp"
#include "../text_util.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::pair<std::string, std::vector<std::string>>> WordFollowerDictionary::build_dictionary(
        const std::vector<std::string>& words) const {

    std::vector<std::string> order;                                    // first-seen order of w1
    std::unordered_map<std::string, std::unordered_map<std::string, int>> freq;

    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        const std::string& w1 = words[i];
        std::string w2 = to_lower(words[i + 1]);
        auto [it, inserted] = freq.try_emplace(w1);
        if (inserted) order.push_back(w1);
        it->second[w2] += 1;
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> dictionary;
    dictionary.reserve(order.size());

    for (const std::string& first_word : order) {
        std::vector<std::pair<std::string, int>> entries(freq[first_word].begin(), freq[first_word].end());
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        std::vector<std::string> top_words;
        const std::size_t n = std::min(static_cast<std::size_t>(max_toplist), entries.size());
        top_words.reserve(n);
        for (std::size_t i = 0; i < n; ++i) top_words.push_back(entries[i].first);

        dictionary.emplace_back(first_word, std::move(top_words));
    }

    return dictionary;
}

std::unordered_map<std::string, std::vector<std::string>> WordFollowerDictionary::run(
        const std::string& input_filename, const std::string& output_filename) {

    std::ifstream in(input_filename);
    std::stringstream buffer;
    buffer << in.rdbuf();

    std::vector<std::string> words_clean;
    for (const std::string& w : split_words(buffer.str())) words_clean.push_back(to_lower(w));

    auto result = build_dictionary(words_clean);

    std::ofstream out(output_filename);
    for (const auto& [word, followers] : result) {
        out << word << " ";
        for (std::size_t i = 0; i < followers.size(); ++i) {
            out << followers[i];
            if (i + 1 < followers.size()) out << " ";
        }
        out << "\n";
    }

    return std::unordered_map<std::string, std::vector<std::string>>(result.begin(), result.end());
}
