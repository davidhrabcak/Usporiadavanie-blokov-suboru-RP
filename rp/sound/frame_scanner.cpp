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

    unsigned long Mp3FrameScanner::getFrameCount(){
        return frames.size();
    }

    FrameData Mp3FrameScanner::getFrame(const size_t index) {
        return frame_data.at(index);
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

bool Mp3FrameScanner::isValidHeader(uint32_t h) {
        if (((h >> 21) & 0x7FF) != 0x7FF) return false;

        uint8_t version = (h >> 19) & 0x3;
        uint8_t layer   = (h >> 17) & 0x3;

        if (version == 0b01) return false;
        if (layer == 0b00) return false;

        return true;
    }

    void Mp3FrameScanner::scanFrames() {
        size_t i = skipID3();
        /*debug print
        cout << "ID3 header: " << i << endl;
        cout << "First bytes after ID3:\n";
        for (int j = 0; j < 100 && i + j < data.size(); ++j) {
            printf("%02X ", data[i + j]);
        }
        cout << endl;

        for (int k = 0; k < 20 && i + k + 4 <= data.size(); ++k) {
            size_t pos = i + k;

            uint32_t raw =
                (uint32_t(data[pos]) << 24) |
                (uint32_t(data[pos+1]) << 16) |
                (uint32_t(data[pos+2]) << 8) |
                (uint32_t(data[pos+3]));

            cout << "pos=" << pos
                 << " raw=0x" << hex << raw << dec
                 << " sync=" << (((raw >> 21) & 0x7FF) == 0x7FF)
                 << endl;
        } */
        while (i + 4 <= data.size()) {
            const uint32_t headerRaw =
                (uint32_t(data[i]) << 24) |
                (uint32_t(data[i+1]) << 16) |
                (uint32_t(data[i+2]) << 8) |
                (uint32_t(data[i+3]));


            if (!isValidHeader(headerRaw)) {
                ++i;
                continue;
            }
            int return_value;
            Header header(headerRaw, return_value);
            if (return_value != 0) {
                /*switch (return_value) {
                    case -1: {
                        cerr << "Failed to decode version, frame " << i << endl;
                        break;
                    }
                    case -2: {
                        cerr << "Failed to decode layer, frame " << i << endl;
                        break;
                    }
                    case -3: {
                        cerr << "Failed to decode bitrate, frame " << i << endl;
                        break;
                    }
                    case -4: {
                        cerr << "Failed to decode sample_rate, frame " << i << endl;
                    }
                        case -5: {
                        cerr << "Failed to decode channel mode, frame " << i << endl;
                        break;
                    }
                        default: {
                        cerr << "Unknown error frame " << i << endl;
                        break;
                    }

                }
                */
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
                                        header.getSampleRate(), header.getChannelMode()});

            i += frameLength; // jump to next frame
        }
    }