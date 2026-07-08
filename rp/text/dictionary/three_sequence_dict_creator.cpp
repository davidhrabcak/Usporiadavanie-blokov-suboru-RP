#include "three_sequence_dict_creator.hpp"
#include "../text_util.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

ThreeSequenceDictionaryCreator::ThreeSequenceDictionaryCreator(const std::string& input_file_path,
                                                                const std::string& coordinates_file) {
    std::ifstream ifile(input_file_path);
    std::stringstream buffer;
    buffer << ifile.rdbuf();
    input_file = buffer.str();
    coordinates = coordinates_file;
}

void ThreeSequenceDictionaryCreator::run() {
    std::ofstream out(coordinates, std::ios::app);

    int i = 0;
    std::vector<std::string> order;                                    // first-seen order of words
    std::unordered_map<std::string, std::vector<int>> dictionary;

    for (const std::string& raw_word : split_words(input_file)) {
        std::string word = clean_word(raw_word);
        auto [it, inserted] = dictionary.try_emplace(word);
        if (inserted) order.push_back(word);
        it->second.push_back(i);
        ++i;
    }

    for (const std::string& key : order) {
        out << key;
        for (int v : dictionary[key]) out << " " << v << " ";
        out << "\n";
    }
}
