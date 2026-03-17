#include <fstream>
#include <vector>
#include <cstdint>
#include <iostream>
#include "Header.hpp"
#include "frame_scanner.hpp"

using namespace std;

class Mp3FrameScanner {
public:
    explicit Mp3FrameScanner(const string& filename) {
        loadFile(filename);
    }

    const vector<FrameInfo>& getFrames() {
        return frames;
    }

    unsigned long getFrameCount(){
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
                        cout << "Failed to decode version, frame " + i << endl;
                        break;
                    }
                    case -2: {
                        cout << "Failed to decode layer, frame " << i << endl;
                        break;
                    }
                    case -3: {
                        cout << "Failed to decode bitrate, frame " << i << endl;
                        break;
                    }
                    case -4: {
                        cout << "Failed to decode sample_rate, frame " << i << endl;
                    }
                        case -5: {
                        cout << "Failed to decode channel mode, frame " << i << endl;
                        break;
                    }
                        default: {
                        cout << "Unknown error frame " << i << endl;
                        break;
                    }

                }
            }

            const int frameLength = header.getFrameLength();

            if (frameLength <= 0) {
                ++i;
                continue;
            }

            // frame fits in file
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