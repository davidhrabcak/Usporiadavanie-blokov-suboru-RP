#ifndef RP_THREE_SEQUENCE_DICT_CREATOR_HPP
#define RP_THREE_SEQUENCE_DICT_CREATOR_HPP

#include <string>

class ThreeSequenceDictionaryCreator {
public:
    ThreeSequenceDictionaryCreator(const std::string& input_file, const std::string& coordinates_file);

    void run();

private:
    std::string input_file;       // full contents of input_file, read eagerly in the constructor
    std::string coordinates;      // path to the coordinates output file
};

#endif
