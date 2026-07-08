#ifndef RP_TWO_SEQUENCE_DICTIONARY_CREATOR_HPP
#define RP_TWO_SEQUENCE_DICTIONARY_CREATOR_HPP

#include <string>
#include <vector>
#include <unordered_map>

/** Creates a 2-word frequency dictionary - for every word, it keeps the
 *  most frequent following words (up to max_toplist of them). */
class WordFollowerDictionary {
public:
    explicit WordFollowerDictionary(int max_toplist = 10) : max_toplist(max_toplist) {}

    /** Creates the dictionary from input_filename and writes it to output_filename */
    std::unordered_map<std::string, std::vector<std::string>> run(
        const std::string& input_filename,
        const std::string& output_filename = "dictionary_custom.txt");

private:
    int max_toplist;

    /** Internal method that builds the dictionary. Returned in first-seen
     *  order of the first word, matching Python dict insertion order. */
    std::vector<std::pair<std::string, std::vector<std::string>>> build_dictionary(
        const std::vector<std::string>& words) const;
};

#endif
