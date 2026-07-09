#ifndef RP_TWO_SEQUENCE_DICTIONARY_HPP
#define RP_TWO_SEQUENCE_DICTIONARY_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include "base_dictionary.hpp"

/** Dictionary that holds words and up to 10 most frequent subsequent words */
class FrequencyDictionary : public BaseDictionary {
public:
    /** Loads created dictionary from a file */
    void load(const std::string& source) override;

    /** Returns list associated with first_word, empty if absent */
    std::vector<std::string> get_following_words(const std::string& first_word) const;

    /** Returns whether second_word is among the followers recorded for first_word */
    bool contains(const std::string& first_word, const std::string& second_word) const;

    std::unordered_map<std::string, std::vector<std::string>> data;
};

#endif
