#include <fstream>
#include <vector>
#include <cstdint>
#include <iostream>
#include "Header.hpp"

using namespace std;

struct FrameData {
    string mpeg_version;
    string layer;
    bool crc;
    int bitrate;
    int sample_rate;
    string channel_mode;
};

struct FrameInfo {
    size_t position;   // byte offset in file
    int length;     // frame length in bytes
    FrameData frame_data;
};

ostream& operator<<(ostream &os, const FrameData& data) {
    os << "MPEG: " << data.mpeg_version << endl;
    os << "Layer: " << data.layer << endl;
    os << "CRC: " << ((data.crc) ? "Used" : "Not used") << endl;
    os << "Bitrate: " << data.bitrate << endl;
    os << "Sample rate: " << data.sample_rate << endl;
    os << "Channel mode: " << data.channel_mode << endl;
    return os;
}

class Mp3FrameScanner {
public:
    explicit Mp3FrameScanner(const string& filename) {
        loadFile(filename);
    }

    const vector<FrameInfo>& getFrames() const {
        return frames;
    }

    unsigned long getFrameCount() const {
        return frames.size();
    }

    FrameData getFrame(const size_t index) {
        return frame_data.at(index);
    }

private:
    vector<uint8_t> data;
    vector<FrameInfo> frames;
    vector<FrameData> frame_data;

    void loadFile(const string& filename) {
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

    size_t skipID3() const {
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

    static bool isValidHeader(uint32_t headerRaw) {
        // sync bits check
        return ((headerRaw >> 21) & 0x7FF) == 0x7FF;
    }

    void scanFrames() {
        size_t i = skipID3();

        while (i + 4 <= data.size()) {
            const unsigned int headerRaw =
                (data[i] << 24) |
                (data[i+1] << 16) |
                (data[i+2] << 8) |
                (data[i+3]);

            if (!isValidHeader(headerRaw)) {
                ++i;
                continue;
            }
            int return_value;
            Header header(headerRaw, return_value);
            if (return_value != 0) {
                switch (return_value) {
                    case -1: {
                        //TODO dopisat vypis kazdej chyby,  scanFrames by tiez mala vratit bool
                    }
                }
            }

            const int frameLength = header.getFrameLength();

            if (frameLength <= 0) {
                ++i;
                continue;
            }

            // Ensure frame fits in file
            if (i + frameLength > data.size()) {
                break;
            }

            frames.push_back({i, frameLength});
            frame_data.push_back({header.getMpegVersion(), header.getLayer(),
                                        header.hasCRC(), header.getBitrate(),
                                        header.getSampleRate(), header.getChannelMode()});

            i += frameLength; // jump to next frame
        }
    }
};

int main(int argc, char** argv) {
    auto s = Mp3FrameScanner("../../sound/file_example_MP3_700KB.mp3");
    cout << s.getFrames().data()->length << endl;
    cout << s.getFrame(1000);
    cout << s.getFrameCount() << endl;

}