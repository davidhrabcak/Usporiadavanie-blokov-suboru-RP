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




#define CHUNK_SIZE 1024
#define MAX_OFFSET 1024
#define VALID_SUBSEQUENT_FRAMES 5
#define BUFFER_SIZE 3800

using namespace std;

struct State {
    vector<int> order; // indexes of chunks in current order
    vector<uint8_t> buffer;  // last few frames
    vector<bool> used;
};

bool check(const Header& h, const Header& other) {
    //if (h.getBitrate() != other.getBitrate()) return false; // doesn't work for VBR
    if (h.getSampleRate() != other.getSampleRate()) return false;
    if (h.getLayer() != other.getLayer()) return false;
    if (h.getMpegVersion() != other.getMpegVersion()) return false;

    return true;
}

bool isValidContinuation(const vector<uint8_t>& buffer,
                         const vector<uint8_t>& nextChunk) {

    vector<uint8_t> temp;

    size_t take = min(buffer.size(), size_t(BUFFER_SIZE));
    temp.insert(temp.end(), buffer.end() - take, buffer.end());
    temp.insert(temp.end(), nextChunk.begin(), nextChunk.end());


    for (int start = 0; start < MAX_OFFSET && start + 4 <= (int)temp.size(); ++start) {

        const uint32_t h =
            (static_cast<uint32_t>(temp[start]) << 24) |
            (static_cast<uint32_t>(temp[start + 1]) << 16) |
            (static_cast<uint32_t>(temp[start + 2]) << 8) |
            static_cast<uint32_t>(temp[start + 3]);

        if (!Mp3FrameScanner::isValidHeader(h)) continue;

        int ret;
        Header first(h, ret);
        if (ret != 0) continue;

        size_t i = start;
        int validFrames = 0;

        while (i + 4 <= temp.size()) {

            uint32_t hh =
                (uint32_t(temp[i]) << 24) |
                (uint32_t(temp[i + 1]) << 16) |
                (uint32_t(temp[i + 2]) << 8) |
                (uint32_t(temp[i + 3]));

            if (!Mp3FrameScanner::isValidHeader(hh)) break;

            int ret2;
            Header h2(hh, ret2);
            if (ret2 != 0) break;

            if (!check(first, h2)) break;

            int len = h2.getFrameLength();
            if (len <= 0) break;
            if (i + len > temp.size()) break;

            i += len;
            validFrames++;
        }

        if (validFrames >= VALID_SUBSEQUENT_FRAMES) return true; // TODO dependant on buffer size - find the right balance
    }

    return false;
}
uint64_t counter = 0;

void appendAndTrim(vector<uint8_t>& buffer,
                   const vector<uint8_t>& chunk) {

    buffer.insert(buffer.end(), chunk.begin(), chunk.end());

    const size_t MAX_BUF = 4096; // enough?

    if (buffer.size() > MAX_BUF) {
        buffer.erase(buffer.begin(),
                     buffer.begin() + (buffer.size() - MAX_BUF));
    }
}

bool dfs(State& state, vector<vector<uint8_t>>& chunks, vector<int>& result) {
    if (state.order.size() == chunks.size()) {
        result = state.order;
        return true;
    }

    vector<int> candidates; // all that pass check
    for (int i = 0; i < (int)chunks.size(); ++i) {
        if (state.used[i]) continue;
        if (isValidContinuation(state.buffer, chunks[i])) {
            candidates.push_back(i);
        }
    }

    if (candidates.empty()) return false;

    for (int i : candidates) {
        state.used[i] = true;
        state.order.push_back(i);

        vector<uint8_t> oldBuffer = state.buffer;
        appendAndTrim(state.buffer, chunks[i]);

        if (state.order.size() % 10 == 0 || state.order.size() < 40) {
            counter++;
            if (counter % 2767 == 0) {
                cout << "Depth: " << state.order.size() << "/" << chunks.size() << endl;
                counter = 0;
            }

        }

        if (dfs(state, chunks, result)) return true;

        state.buffer = oldBuffer;
        state.order.pop_back();
        state.used[i] = false;
    }
    return false;
}

int main(int argc, char *argv[]) {
    cout << __FILE__ << endl;
    const string filename = "/home/david/Desktop/python/rp/sound/sample-3s.mp3"; //(argc < 2) ? argv[1] : "test.mp3";
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

    while (file.read(reinterpret_cast<char*>(frame.data()), CHUNK_SIZE) || file.gcount() > 0) {
        size_t readCount = file.gcount();
        chunks.emplace_back(frame.begin(), frame.begin() + readCount);
    }
    unsigned seed = 42;
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));

    State state;
    state.used.resize(chunks.size(), false);
    state.order.push_back(0);
    state.used[0] = true;
    state.buffer = chunks[0];

    vector<int> result;

    cout << "Total chunks loaded: " << chunks.size() << " (Expected approx " << (52000/CHUNK_SIZE) << ")" << endl;
    if (dfs(state, chunks, result)) {
        cout << "Reconstruction found: " << result.size() << " indexes" << endl;

        ofstream outputFile("output.mp3", ios::binary | ios::trunc);

        if (!outputFile.is_open()) {
            cerr << "Error opening output file" << endl;
            return 1;
        }

        size_t totalBytesWritten = 0;

        for (int index : result) {
            outputFile.write(reinterpret_cast<const char*>(chunks[index].data()), chunks[index].size());
            totalBytesWritten += chunks[index].size();
        }

        outputFile.close();
        cout << "Written " << totalBytesWritten << " bytes to output.mp3" << endl;
    } else {
        cout << "Failed to reconstruct." << endl;
    }
    return 0;
}