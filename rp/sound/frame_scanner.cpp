#include <fstream>
#include <vector>
#include <cstdint>
#include <iostream>
#include "Header.hpp"
#include "frame_scanner.hpp"

using namespace std;

    Mp3FrameScanner::Mp3FrameScanner(const string& filename) {
        loadFile(filename);
    }

    const vector<FrameInfo>& Mp3FrameScanner::getFrames() {
        return frames;
    }

    unsigned long Mp3FrameScanner::getFrameCount() const {
        return frames.size();
    }

    FrameData Mp3FrameScanner::getFrame(const size_t index) {
        return frame_data.at(index);
    }

    vector<vector<uint8_t>> Mp3FrameScanner::getRawChunks() const {
        vector<vector<uint8_t>> chunks;
        chunks.reserve(frame_data.size());
        for (const FrameData& fd : frame_data) chunks.push_back(fd.rawBits);
        return chunks;
    }


    void Mp3FrameScanner::loadFile(const string& filename) {
        ifstream file(filename, ios::binary);
        if (!file) {
            cout << "Failed to open file" << endl;
            return;
        }

        data = vector<uint8_t>(
            istreambuf_iterator(file),
            istreambuf_iterator<char>()
        );

        scanFrames();
    }

    size_t Mp3FrameScanner::skipID3() const {
        if (data.size() < 10) return 0;

        if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
            const size_t size =
                ((data[6] & 0x7F) << 21) |
                ((data[7] & 0x7F) << 14) |
                ((data[8] & 0x7F) << 7) |
                (data[9] & 0x7F);

            return size + 10;
        }

        return 0;
    }

bool Mp3FrameScanner::isValidHeader(uint32_t headerRaw) {
        if (((headerRaw >> 21) & 0x7FF) != 0x7FF) return false;

        const uint8_t version = (headerRaw >> 19) & 0x3;
        uint8_t layer = (headerRaw >> 17) & 0x3;

        if (version == 0b01 || layer == 0) return false;

        return true;
    }

    void Mp3FrameScanner::scanFrames() {
        size_t i = skipID3();
        while (i + 4 <= data.size()) {
            const uint32_t headerRaw =
                (static_cast<uint32_t>(data[i]) << 24) |
                (static_cast<uint32_t>(data[i + 1]) << 16) |
                (static_cast<uint32_t>(data[i + 2]) << 8) |
                static_cast<uint32_t>(data[i + 3]);


            if (!isValidHeader(headerRaw)) {
                ++i;
                continue;
            }
            int return_value;
            Header header(headerRaw, return_value);
            if (return_value != 0) {
                ++i;
                continue;
            }

            const int frameLength = header.getFrameLength();

            if (frameLength <= 0) {
                ++i;
                continue;
            }

            if (i + frameLength > data.size()) {
                break;
            }
            
            frames.push_back({i, frameLength});
            frame_data.push_back({header.getMpegVersion(), header.getLayer(),
                                        header.hasCRC(), header.getBitrate(),
                header.getSampleRate(), header.getChannelMode(),
                std::vector<uint8_t>(data.begin() + i, data.begin() + i + frameLength)});

            i += frameLength; // jump to next frame
        }
    }