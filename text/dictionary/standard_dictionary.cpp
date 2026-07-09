#include "standard_dictionary.hpp"
#include "../text_util.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>

static std::string trim_lower(const std::string& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    std::string out = s.substr(begin, end - begin);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return out;
}

void StandardDictionary::load(const std::string& source) {
    std::ifstream f(source);
    std::string line;
    while (std::getline(f, line)) {
        std::string word = trim_lower(line);
        if (!word.empty()) data.insert(word);
    }
}

void StandardDictionary::load_from_chunks(const std::vector<std::string>& chunks) {
    for (const std::string& chunk : chunks) {
        for (const std::string& word : split_words(chunk)) {
            data.insert(clean_word(word));
        }
    }
}
