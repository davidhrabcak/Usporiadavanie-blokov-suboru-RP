#ifndef RP_BASE_VALIDATOR_HPP
#define RP_BASE_VALIDATOR_HPP

#include <string>
#include <vector>

class BaseValidator {
public:
    virtual ~BaseValidator() = default;

    virtual bool validate_chunk(const std::string& chunk1, const std::string& chunk2) const = 0;

    virtual bool validate_text(const std::string& text,
                                const std::vector<std::string>& required_chunks) const = 0;
};

#endif
