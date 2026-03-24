//
// Created by david on 3/17/26.
//
#include <random>
#include <algorithm>
#include <string>
#include <cstdint>
#include <iostream>
#include <vector>
#include <fstream>
#include "frame_scanner.hpp"
#include "Header.hpp"
#include "main.hpp"




#define CHUNK_SIZE 512

using namespace std;

bool check(const Header& h, const Header& other) {
    if (h.getBitrate() != other.getBitrate()) return false;
    if (h.getSampleRate() != other.getSampleRate()) return false;
    if (h.getLayer() != other.getLayer()) return false;
    if (h.getMpegVersion() != other.getMpegVersion()) return false;

    return true;
}

int main(int argc, char *argv[]) {
    cout << __FILE__ << endl;
    const string filename = "/home/david/Desktop/python/rp/sound/CBR.mp3"; //(argc < 2) ? argv[1] : "test.mp3";
    const auto s = new Mp3FrameScanner(filename);

    if (s->getFrameCount() == 0) {
        cerr << "No frames found\n";
        return 1;
    }

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

        vector<uint8_t> chunk(frame.begin(), frame.begin() + read);
        chunks.push_back(chunk);
        total += read;
    }
    unsigned seed = 42;
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));

    vector<uint8_t> joined = chunks[0];
    vector<bool> used(chunks.size(), false);
    used[0] = true;

    size_t currentFrameStart = 0;

    while (true) {
        if (currentFrameStart + 4 > joined.size()) break;

        //read header
        uint32_t raw = (joined[currentFrameStart] << 24) |
        (joined[currentFrameStart + 1] << 16) |
        (joined[currentFrameStart + 2] << 8) |
        (joined[currentFrameStart + 3]);

        if (((raw >> 21) & 0x7FF) != 0x7FF) {
            cerr << "Invalid frame" << endl;
            currentFrameStart++;
            continue;
        }

        int err = 0;
        Header h(raw, err);
        if (err != 0) break;

        int frameLength = h.getFrameLength();
        if (frameLength <= 0) break;

        size_t frameEnd = currentFrameStart + frameLength;

        // if complete, move to next frame
        if (frameEnd <= joined.size()) {
            currentFrameStart = frameEnd;
            cout << "Found complete frame" << endl;
            continue;
        }

        size_t missing = frameEnd - joined.size();

        bool found = false;

        //look for corret chunk
        for (size_t i = 0; i < chunks.size(); i++) {
            if (used[i]) continue;

            auto &chunk = chunks[i];

            if (chunk.size() < missing) continue;

            vector<uint8_t> temp = joined;
            temp.insert(temp.end(), chunk.begin(), chunk.end());

            if (frameEnd + 4 <= temp.size()) {
                uint32_t nextRaw =
                    (temp[frameEnd] << 24) |
                    (temp[frameEnd + 1] << 16) |
                    (temp[frameEnd + 2] << 8) |
                    (temp[frameEnd + 3]);

                int err2 = 0;
                Header nextH(nextRaw, err2);

                if (err2 == 0 && check(nextH, h)) {
                    // accept this chunk
                    joined = std::move(temp);
                    used[i] = true;
                    found = true;
                    cout << "Found match" << endl;
                    break;
                }
            }
        }
        if (!found) {
            std::cout << "No valid continuation found" << endl;
            break;
        }
    }
}
