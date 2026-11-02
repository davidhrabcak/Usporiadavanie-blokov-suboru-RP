#include <algorithm>
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

    vector<vector<uint8_t>> Mp3FrameScanner::chunksFromFirstFrame(const long chunkSize) const {
        vector<vector<uint8_t>> chunks;
        if (frames.empty() || chunkSize <= 0) return chunks;
        for (size_t pos = frames[0].position; pos < data.size(); pos += chunkSize)
            chunks.emplace_back(data.begin() + pos, data.begin() + min(data.size(), pos + chunkSize));
        return chunks;
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
            istreambuf_iterator<char>(file),
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

    void Mp3FrameScanner::scanFrames() {
        size_t i = skipID3();
        while (i + 4 <= data.size()) {
            Header header;
            if (!Header::parse(&data[i], header)) {
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
            frame_data.push_back({std::vector<uint8_t>(data.begin() + i, data.begin() + i + frameLength)});

            i += frameLength; // jump to next frame
        }
    }