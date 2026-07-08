#ifndef RP_TEXT_UTIL_HPP
#define RP_TEXT_UTIL_HPP

#include <string>
#include <vector>
#include <cctype>

/** Strips every character that is not A-Z, a-z or '-' and lowercases the rest.
 *  Mirrors the repeated `re.sub(r"[^A-Za-z\-]", '', word).lower()` pattern
 *  used throughout the original Python dictionary/validation modules. */
inline std::string clean_word(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-') {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

/** Splits on any run of whitespace, discarding empty tokens - equivalent to Python's str.split(). */
inline std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> words;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > start) words.push_back(text.substr(start, i - start));
    }
    return words;
}

#endif
