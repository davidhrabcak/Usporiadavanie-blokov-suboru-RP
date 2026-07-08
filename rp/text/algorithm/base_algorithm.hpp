#ifndef RP_BASE_ALGORITHM_HPP
#define RP_BASE_ALGORITHM_HPP

#include <string>
#include <vector>
#include <optional>

class BaseAlgorithm {
public:
    virtual ~BaseAlgorithm() = default;

    virtual std::vector<std::string> reconstruct_all(const std::string& output_file) = 0;

    virtual std::optional<std::string> reconstruct_one(const std::string& output_file) = 0;
};

#endif
