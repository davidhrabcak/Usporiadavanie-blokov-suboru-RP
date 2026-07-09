#include "three_sequence_dictionary.hpp"
#include "../text_util.hpp"
#include <fstream>

void ThreeSequenceDictionary::load(const std::string& source_file, const std::string& coordinates_file) {
    {
        std::ifstream f1(source_file);
        std::string line;
        while (std::getline(f1, line)) {
            for (const std::string& word : split_words(line)) {
                dictionary.push_back(clean_word(word));
            }
        }
    }

    std::ifstream f2(coordinates_file);
    std::string line;
    while (std::getline(f2, line)) {
        std::vector<std::string> words = split_words(line);
        if (words.empty()) continue;
        std::vector<int> coords;
        coords.reserve(words.size() - 1);
        for (std::size_t i = 1; i < words.size(); ++i) coords.push_back(std::stoi(words[i]));
        coordinates[words[0]] = std::move(coords);
    }
}

bool ThreeSequenceDictionary::contains(const std::string& middle_word_raw,
                                       const std::vector<std::string>& first_last_raw) const {
    const std::string middle_word = clean_word(middle_word_raw);
    std::vector<std::string> first_last;
    first_last.reserve(first_last_raw.size());
    for (const std::string& w : first_last_raw) first_last.push_back(clean_word(w));

    auto it = coordinates.find(middle_word);
    if (it == coordinates.end()) return false;

    for (int coord : it->second) {
        if (coord > 0 && coord < static_cast<int>(dictionary.size()) - 1) {
            if (dictionary[coord - 1] == first_last[0] && dictionary[coord + 1] == first_last[1]) {
                return true;
            }
        }
    }
    return false;
}
