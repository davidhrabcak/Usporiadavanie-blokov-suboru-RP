#include "mp3/frame_scanner.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include "mp3/Header.hpp"

using namespace std;

Mp3FrameScanner::Mp3FrameScanner(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file) {
        cerr << "Failed to open " << filename << "\n";
        return;
    }
    data.assign(istreambuf_iterator<char>(file), istreambuf_iterator<char>());
    scanFrames();
}

vector<vector<uint8_t>> Mp3FrameScanner::chunksFromFirstFrame(const long chunkSize) const {
    vector<vector<uint8_t>> chunks;
    if (frames.empty() || chunkSize <= 0) return chunks;
    const size_t end = audioEnd();
    for (size_t pos = frames[0].position; pos < end; pos += chunkSize)
        chunks.emplace_back(data.begin() + pos, data.begin() + min(end, pos + chunkSize));
    return chunks;
}

size_t Mp3FrameScanner::skipID3() const {
    if (data.size() < 10 || memcmp(data.data(), "ID3", 3) != 0) return 0;
    const size_t size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) |
                        ((data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    return size + 10;
}

// End of the audio: the file size minus the tags appended after the last frame, each found by
// its own footer, from the end backwards: ID3v1 (128 bytes, "TAG"), Lyrics3v2 (6-digit size +
// "LYRICS200"), APEv2 (32-byte footer "APETAGEX" with the tag size, + 32 when it has a header).
// They are not MPEG audio, so a chunk made of them can't be placed by any rule here (it broke
// the last junction of 19 musiclib tracks at 4 KB), and they are as easy to find by signature
// as the leading ID3v2 tag that skipID3 drops.
size_t Mp3FrameScanner::audioEnd() const {
    const size_t first = frames.empty() ? 0 : frames[0].position;
    size_t end = data.size();
    auto has = [&](const size_t back, const char* sig) {
        const size_t len = strlen(sig);
        return end >= first + back && back >= len && memcmp(&data[end - back], sig, len) == 0;
    };
    if (has(128, "TAG")) end -= 128;
    if (has(9, "LYRICS200") && end >= first + 15) {
        size_t size = 0;
        bool digits = true;
        for (size_t k = end - 15; k < end - 9; ++k) {
            digits = digits && data[k] >= '0' && data[k] <= '9';
            size = size * 10 + (data[k] - '0');
        }
        if (digits && end >= first + size + 15) end -= size + 15;
    }
    if (has(32, "APETAGEX")) {
        const uint8_t* f = &data[end - 32];
        size_t size = f[12] | (f[13] << 8) | (f[14] << 16) | (static_cast<size_t>(f[15]) << 24);
        if (f[23] & 0x80) size += 32;   // flags bit 31: the tag has a header too
        if (size >= 32 && end >= first + size) end -= size;
    }
    return end;
}

void Mp3FrameScanner::scanFrames() {
    size_t i = skipID3();
    while (i + 4 <= data.size()) {
        Header header;
        if (!Header::parse(&data[i], header) || header.getFrameLength() <= 0) {
            ++i;
            continue;
        }
        const int frameLength = header.getFrameLength();
        if (i + frameLength > data.size()) break;
        frames.push_back({i, static_cast<int>(frameLength)});
        i += frameLength;
    }
}
