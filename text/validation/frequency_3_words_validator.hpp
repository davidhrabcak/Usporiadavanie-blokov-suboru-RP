#ifndef RP_FREQUENCY_3_WORDS_VALIDATOR_HPP
#define RP_FREQUENCY_3_WORDS_VALIDATOR_HPP

#include "base_validator.hpp"
#include "../dictionary/three_sequence_dictionary.hpp"

class FrequencyThreeWordsValidator : public BaseValidator {
public:
    FrequencyThreeWordsValidator(const ThreeSequenceDictionary& dictionary, const std::vector<std::string>& chunks)
        : dictionary(dictionary), all_chunks(chunks) {}

    bool validate_chunk(const std::string& chunk1, const std::string& chunk2) const override;
    bool validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const override;

private:
    const ThreeSequenceDictionary& dictionary;
    std::vector<std::string> all_chunks;
};

#endif
