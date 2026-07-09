#include "segment.hpp"
#include <cstdlib>
#include <fstream>

void Segmenter::segment() const {
    std::system(("split -b" + std::to_string(segment_size) + " -d " + input_file + " segment_").c_str());
    std::system(("ls segment_* | shuf | xargs -I {} sh -c 'cat {}; echo' >" + output_file).c_str());
    std::system("rm segment_*");
}

std::vector<std::string> Segmenter::get_chunks() const {
    std::vector<std::string> result;
    std::ifstream f(output_file);
    std::string raw;
    // Each line produced by segment() always ends with '\n' (the "echo" after
    // every "cat" guarantees it), so raw here is always the line without that
    // trailing newline - equivalent to Python's `line[:-1]` for a non-empty
    // `line`. A line that was just "\n" (raw == "") is kept as the literal
    // newline character, matching the original `else: result.append(line)`.
    while (std::getline(f, raw)) {
        result.push_back(raw.empty() ? "\n" : raw);
    }
    return result;
}
