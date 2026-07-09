#include <cassert>
#include <algorithm>
#include <iostream>
#include "../dictionary/three_sequence_dictionary.hpp"
#include "../validation/frequency_3_words_validator.hpp"

// NOTE: this mirrors test_frequency_dictionary_validator.py, which already
// depended on a "test/frequency.txt" fixture that does not exist in the repo
// (only an empty test/coordinates.txt is present) - the Python version would
// crash on open() the same way this one will fail the "this" assertion below.
static void test_validate_chunk() {
    ThreeSequenceDictionary dic;
    dic.load("test/frequency.txt", "test/coordinates.txt");
    assert(std::find(dic.dictionary.begin(), dic.dictionary.end(), "this") != dic.dictionary.end());

    std::vector<std::string> chunks = {"This", "is", "the", "sequence",
                                        "that", "was", "used", "for", "testing."};
    FrequencyThreeWordsValidator validator(dic, chunks);
    assert(dic.contains("the", {"is", "sentence"}));
    assert(!dic.contains("was", {"used", "that"}));

    assert(validator.validate_chunk("the sentence ", "that was"));
    assert(!validator.validate_chunk("the sentence", "that was used"));
    assert(validator.validate_chunk("the sentence ", " that was"));
    assert(validator.validate_chunk("the sente", "nce that"));
}

int main() {
    test_validate_chunk();
    std::cout << "test_frequency_dictionary_validator: all tests passed\n";
    return 0;
}
