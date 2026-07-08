#include "two_sequence_dictionary.hpp"
#include "../text_util.hpp"
#include <fstream>
#include <algorithm>

void FrequencyDictionary::load(const std::string& source) {
    std::ifstream f(source);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> words = split_words(line);
        if (words.size() >= 2) {
            data[words[0]] = std::vector<std::string>(words.begin() + 1, words.end());
        }
    }
}

std::vector<std::string> FrequencyDictionary::get_following_words(const std::string& first_word) const {
    auto it = data.find(first_word);
    if (it == data.end()) return {};
    return it->second;
}

bool FrequencyDictionary::contains(const std::string& first_word, const std::string& second_word) const {
    auto it = data.find(first_word);
    if (it == data.end()) return false;
    return std::find(it->second.begin(), it->second.end(), second_word) != it->second.end();
}
