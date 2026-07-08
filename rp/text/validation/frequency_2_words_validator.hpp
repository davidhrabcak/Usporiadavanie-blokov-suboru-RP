#ifndef RP_FREQUENCY_2_WORDS_VALIDATOR_HPP
#define RP_FREQUENCY_2_WORDS_VALIDATOR_HPP

#include "base_validator.hpp"
#include "../dictionary/two_sequence_dictionary.hpp"

class FrequencyValidator : public BaseValidator {
public:
    FrequencyValidator(const FrequencyDictionary& dictionary, const std::vector<std::string>& chunks)
        : dictionary(dictionary), all_chunks(chunks) {}

    bool validate_chunk(const std::string& chunk1, const std::string& chunk2) const override;
    bool validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const override;

private:
    const FrequencyDictionary& dictionary;
    std::vector<std::string> all_chunks;
};

#endif
