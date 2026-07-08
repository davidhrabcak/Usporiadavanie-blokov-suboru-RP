#include <cassert>
#include <algorithm>
#include <iostream>
#include "../dictionary/standard_dictionary.hpp"
#include "../validation/standard_validator.hpp"

static void test_validate_chunk() {
    StandardDictionary std_dict;
    std_dict.load("dict_en.txt");
    assert(std_dict.data.count("first") > 0);

    StandardValidator validator(std_dict);
    assert(validator.validate_chunk("This is the fir", "st sentence."));

    assert(!validator.validate_chunk("a pareng", "fdgj l"));

    assert(validator.validate_chunk("first part ", " second part"));
    assert(validator.validate_chunk("first part ", "second part"));
    assert(validator.validate_chunk("first part", " second part"));
    assert(!validator.validate_chunk("first part", "second part"));
}

static void test_validate_text() {
    StandardDictionary std_dict;
    std_dict.load("dict_en.txt");

    std::vector<std::string> required_chunks = {"This ", " is", " the ", "second", " sentence."};
    auto joined = [](const std::vector<std::string>& chunks, const std::string& sep = "") {
        std::string out;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            out += chunks[i];
            if (i + 1 < chunks.size()) out += sep;
        }
        return out;
    };

    StandardValidator validator(std_dict);
    assert(validator.validate_text(joined(required_chunks), required_chunks));
    assert(validator.validate_text("This is the second sentence.", required_chunks));
    assert(validator.validate_text("This the is sentence. second", required_chunks));
    assert(!validator.validate_text("This 3is the !#$sentence. second_ )!+", required_chunks));
    assert(!validator.validate_text("This is the sentence.", required_chunks));

    required_chunks = {"T", "his ", "is", " in", " the", " in", " correct ", "ord", "er"};
    assert(validator.validate_text(joined(required_chunks), required_chunks));

    std::vector<std::string> all_but_last(required_chunks.begin(), required_chunks.end() - 1);
    assert(!validator.validate_text(joined(all_but_last, " "), required_chunks));

    std::sort(required_chunks.begin(), required_chunks.end());
    assert(!validator.validate_text(joined(required_chunks), required_chunks));
}

int main() {
    test_validate_chunk();
    test_validate_text();
    std::cout << "test_standard_dictionary_validator: all tests passed\n";
    return 0;
}
