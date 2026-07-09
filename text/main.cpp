#include <iostream>
#include <sstream>
#include "dictionary/three_sequence_dictionary.hpp"
#include "dictionary/three_sequence_dict_creator.hpp"
#include "validation/frequency_3_words_validator.hpp"
#include "segment.hpp"
#include "algorithm/backtrack.hpp"

/** Renders a vector<string> the way Python prints a list of strings. */
static std::string repr_list(const std::vector<std::string>& items) {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        os << "'" << items[i] << "'";
        if (i + 1 < items.size()) os << ", ";
    }
    os << "]";
    return os.str();
}

int main() {
    Segmenter seg("in.txt", "chunk_file.txt", 64);
    seg.segment();
    std::vector<std::string> chunks = seg.get_chunks();

    ThreeSequenceDictionaryCreator creator("data.txt", "coordinates.txt");
    creator.run();
    ThreeSequenceDictionary dictionary;
    dictionary.load("data.txt", "coordinates.txt");
    FrequencyThreeWordsValidator validator(dictionary, chunks);

    Backtrack reconstructor(validator, chunks);
    std::vector<std::string> result = reconstructor.reconstruct_all("found.txt");
    if (!result.empty()) {
        std::cout << "Found reconstruction: " << repr_list(result) << "\n";
        std::cout << result.size() << "\n";
    } else {
        std::cout << "No valid reconstruction found\n";
    }

    return 0;
}
