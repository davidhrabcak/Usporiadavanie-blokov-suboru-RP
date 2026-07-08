#ifndef RP_THREE_SEQUENCE_DICTIONARY_HPP
#define RP_THREE_SEQUENCE_DICTIONARY_HPP

#include <string>
#include <vector>
#include <unordered_map>

/** Dictionary that saves three word sequences instead of only 2.
 *  Not a BaseDictionary - its load() takes two source files instead of one,
 *  matching the original Python class which does not inherit BaseDictionary. */
class ThreeSequenceDictionary {
public:
    /** Loads the dictionary from source files */
    void load(const std::string& source_file, const std::string& coordinates_file);

    /** Returns true if there is an entry with middle_word where first_last
     *  are the words immediately before and after it in the sequence */
    bool contains(const std::string& middle_word, const std::vector<std::string>& first_last) const;

    std::vector<std::string> dictionary;
    std::unordered_map<std::string, std::vector<int>> coordinates;
};

#endif
