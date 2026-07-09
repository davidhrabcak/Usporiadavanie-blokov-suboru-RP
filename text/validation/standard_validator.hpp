#ifndef RP_STANDARD_VALIDATOR_HPP
#define RP_STANDARD_VALIDATOR_HPP

#include "base_validator.hpp"
#include "../dictionary/standard_dictionary.hpp"

class StandardValidator : public BaseValidator {
public:
    explicit StandardValidator(const StandardDictionary& dictionary) : dictionary(dictionary) {}

    bool validate_chunk(const std::string& chunk1, const std::string& chunk2) const override;
    bool validate_text(const std::string& text, const std::vector<std::string>& required_chunks) const override;

private:
    const StandardDictionary& dictionary;
};

#endif
