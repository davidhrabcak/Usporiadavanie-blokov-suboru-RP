#include <iostream>
#include <sstream>
#include "dictionary/standard_dictionary.hpp"
#include "validation/standard_validator.hpp"
#include "segment.hpp"
#include "algorithm/backtrack.hpp"

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
    Segmenter seg("in.txt", "chunk_file.txt", 32);
    seg.segment();

    StandardDictionary dictionary;
    dictionary.load("dict_en.txt");
    StandardValidator validator(dictionary);
    Backtrack reconstructor(validator, seg.get_chunks());

    std::cout << repr_list(reconstructor.reconstruct_all("found.txt")) << "\n";

    std::optional<std::string> one = reconstructor.reconstruct_one("found.txt");
    std::cout << (one ? *one : "None") << "\n";

    return 0;
}
