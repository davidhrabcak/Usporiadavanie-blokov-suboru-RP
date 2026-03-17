//
// Created by david on 3/17/26.
//
#include <random>
#include <algorithm>
#include <string>
#include <cstdint>
#include <vector>
#include <fstream>
#include "frame_scanner.hpp"
#include "Header.hpp"
#include "main.hpp"



#define CHUNK_SIZE 100

using namespace std;

int main(int argc, char *argv[]) {
    const string filename = (argc < 2) ? argv[1] : "test.mp3";
    const auto s = new Mp3FrameScanner(filename);
    FrameData first = s->getFrame(0);
    long offset = s->getFrames()[0].position; // in bytes

    ifstream file(filename, ios::binary);
    file.seekg(offset, ios::beg);
    vector<vector<uint8_t>> chunks;

    vector<uint8_t> frame(CHUNK_SIZE);
    size_t total = 0;

    while (!file.eof()) {
        file.read(reinterpret_cast<istream::char_type *>(frame.data()), CHUNK_SIZE);
        size_t read = file.gcount();

        chunks.push_back(frame);
        total += read;
    }

    unsigned seed = 42;
    shuffle(chunks.begin(), chunks.end(), default_random_engine(seed));

    // ...
}
