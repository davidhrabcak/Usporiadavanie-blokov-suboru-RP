#include "frequency_3_words_validator.hpp"
#include "../text_util.hpp"

bool FrequencyThreeWordsValidator::validate_chunk(const std::string& chunk1, const std::string& chunk2) const {
    std::vector<std::string> ch1 = split_words(chunk1);
    std::vector<std::string> ch2 = split_words(chunk2);
    if (ch1.size() < 2 || ch2.size() < 2) return true;

    if ((!chunk1.empty() && chunk1.back() == ' ') || (!chunk2.empty() && chunk2.front() == ' ')) {
        const std::string& w0 = ch1[ch1.size() - 2];
        const std::string& w1 = ch1.back();
        const std::string& w2 = ch2.front();
        const std::string& w3 = ch2[1];
        return dictionary.contains(w2, {w1, w3}) && dictionary.contains(w1, {w0, w2});
    }

    const std::string& w0 = ch1[ch1.size() - 2];
    std::string w1 = ch1.back() + ch2.front();
    const std::string& w2 = ch2[1];
    return dictionary.contains(w1, {w0, w2});
}

bool FrequencyThreeWordsValidator::validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const {
    for (const std::string& chunk : required_chunks) {
        if (text.find(chunk) == std::string::npos) return false;
    }
    return true;
}
