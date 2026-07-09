#include "standard_validator.hpp"
#include "../text_util.hpp"
#include <algorithm>
#include <cctype>

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool StandardValidator::validate_chunk(const std::string& chunk1, const std::string& chunk2) const {
    if ((!chunk1.empty() && chunk1.back() == ' ') || (!chunk2.empty() && chunk2.front() == ' ')) {
        return true;
    }

    std::vector<std::string> words1 = split_words(chunk1);
    std::vector<std::string> words2 = split_words(chunk2);
    if (words1.size() < 2 || words2.size() < 2) return true;

    std::string combined = to_lower(words1.back() + words2.front());
    return dictionary.data.count(combined) > 0;
}

bool StandardValidator::validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const {
    for (const std::string& chunk : required_chunks) {
        if (text.find(chunk) == std::string::npos) return false;
    }

    for (const std::string& raw : split_words(text)) {
        std::string word = clean_word(raw);
        if (!word.empty() && dictionary.data.count(word) == 0) return false;
    }

    return true;
}
