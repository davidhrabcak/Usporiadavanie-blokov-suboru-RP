#ifndef RP_BACKTRACK_HPP
#define RP_BACKTRACK_HPP

#include <fstream>
#include "base_algorithm.hpp"
#include "../validation/base_validator.hpp"

/** Performs backtracking algorithm */
class Backtrack : public BaseAlgorithm {
public:
    Backtrack(const BaseValidator& validator, const std::vector<std::string>& all_chunks)
        : validator(validator), all_chunks(all_chunks) {}

    /** Reconstructs all valid texts based on validator */
    std::vector<std::string> reconstruct_all(const std::string& output_file) override;

    /** Reconstructs first valid text based on validator */
    std::optional<std::string> reconstruct_one(const std::string& output_file) override;

private:
    const BaseValidator& validator;
    std::vector<std::string> all_chunks;

    /** helper backtracking function */
    std::optional<std::string> backtrack(const std::string& current_text,
                                          std::vector<bool>& used,
                                          std::ofstream& file,
                                          std::vector<std::string>& results,
                                          bool find_all);
};

#endif
