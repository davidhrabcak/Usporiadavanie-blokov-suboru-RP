#include "frequency_2_words_validator.hpp"
#include "../text_util.hpp"

bool FrequencyValidator::validate_chunk(const std::string& chunk1, const std::string& chunk2) const {
    std::vector<std::string> ch1 = split_words(chunk1);
    std::vector<std::string> ch2 = split_words(chunk2);

    if (ch1.empty() || ch2.empty()) return true;
    if (ch1.size() < 2 && ch2.size() < 2) return true;

    if (chunk1.back() == ' ' || chunk2.front() == ' ') {
        return dictionary.contains(ch1.back(), ch2.front());
    }

    std::string new_word = ch1.back() + ch2.front();
    if (dictionary.data.count(new_word) == 0) return false;
    if (ch1.size() > 2) {
        if (!dictionary.contains(ch1[ch1.size() - 2], new_word)) return false;
    }
    if (ch2.size() > 2) {
        if (!dictionary.contains(new_word, ch2[1])) return false;
    }
    return true;
}

bool FrequencyValidator::validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const {
    for (const std::string& chunk : required_chunks) {
        if (text.find(chunk) == std::string::npos) return false;
    }
    return true;
}
