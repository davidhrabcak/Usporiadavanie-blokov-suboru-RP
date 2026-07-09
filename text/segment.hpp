#ifndef RP_SEGMENT_HPP
#define RP_SEGMENT_HPP

#include <string>
#include <vector>

/** Handles segmentation of the original file and loading segment chunks */
class Segmenter {
public:
    Segmenter(std::string input_file, std::string output_file, int segment_size)
        : input_file(std::move(input_file)), output_file(std::move(output_file)), segment_size(segment_size) {}

    /** Segments input file into chunks of segment_size and saves them into the output file */
    void segment() const;

    /** Return chunks in output file */
    std::vector<std::string> get_chunks() const;

private:
    std::string input_file;
    std::string output_file;
    int segment_size;
};

#endif
