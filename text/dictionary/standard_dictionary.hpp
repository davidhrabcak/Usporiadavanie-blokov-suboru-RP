#ifndef RP_STANDARD_DICTIONARY_HPP
#define RP_STANDARD_DICTIONARY_HPP

#include <string>
#include <vector>
#include <unordered_set>
#include "base_dictionary.hpp"

/** Creates dictionary in 2 possible ways:
 *   - loading from a file
 *   - creating it from complete words inside chunks */
class StandardDictionary : public BaseDictionary {
public:
    /** Loads dictionary from source file */
    void load(const std::string& source) override;

    /** Creates a dictionary from words inside chunks */
    void load_from_chunks(const std::vector<std::string>& chunks);

    std::unordered_set<std::string> data;
};

#endif
