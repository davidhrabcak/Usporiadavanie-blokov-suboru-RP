#include <bits/stdc++.h>

using namespace std;

class Header {
public:
    explicit Header(uint32_t input) {
        raw = input;
        if (synchValid()) {
            decode();
        } else {
            cout << "Invalid input" << endl;
        }
    }

    uint32_t getRaw() const { return raw;}
    string getMpegVersion() const { return mpegVersion; }
    string getLayer() const { return layer; }
    bool hasCRC() const { return protectionBit == 0; }
    int getBitrate() const { return bitrate; } // kbps
    int getSampleRate() const { return sampleRate; } // Hz
    bool hasPadding() const { return padding; }
    string getChannelMode() const { return channelMode; }
    int getFrameLength() const { return frameLength; } // bytes

private:
    uint32_t raw;
    string mpegVersion;
    string layer;
    uint8_t protectionBit;
    int bitrate;
    int sampleRate;
    bool padding;
    string channelMode;
    int frameLength;

    bool synchValid() const {
        return ((raw >> 21) & 0x7FF) == 0x7FF;
    }

    void decode() {
        uint8_t versionID = (raw >> 19) & 0x3;
        uint8_t layerIndex = (raw >> 17) & 0x3;
        protectionBit = (raw >> 16) & 0x1;
        uint8_t bitrateIndex = (raw >> 12) & 0xF;
        uint8_t sampleIndex = (raw >> 10) & 0x3;
        padding = ((raw >> 9) & 0x1);
        uint8_t channelIndex = (raw >> 6) & 0x3;

        decodeVersion(versionID);
        decodeLayer(layerIndex);
    }

    void decodeVersion(const uint8_t versionID) {
        switch (versionID) {
            case 0x00: mpegVersion = "MPEG 2.5"; break;
            case 0x10: mpegVersion = "MPEG 2"; break;
            case 0x11: mpegVersion = "MPEG 1"; break;
            default: mpegVersion = "unknown versionID " + to_string(versionID); break;
        }
    }

    void decodeLayer(const uint8_t layerIndex) {
        switch (layerIndex) {
            case 0x01: layer = "Layer III"; break;
            case 0x10: layer = "Layer II"; break;
            case 0x11: layer = "Layer I"; break;
            default: layer = "unknown layerIndex " + to_string(layerIndex); break;
        }
    }

    void decodeBitrate(const uint8_t bitrateIndex, const uint8_t versionID, const uint8_t layerIndex) {
        static const int v1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224,
                                    256, 288, 320, 352, 384, 416, 448, -1};
        static const int v1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                    128, 160, 192, 224, 256, 320, -1};
        static const int v1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                    112, 128, 160, 192, 224, 256, 320, -1};
        static const int v2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                    128, 144, 160, 176, 192, 224, 256, -1};
        static const int v2All[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                     64, 80, 96, 112, 144, 160, -1};
        // 0 = free format - bitrate is different to predetermined values
        // L3 could have VBR!!

        if (bitrateIndex > 15) {
            cout << "Invalid bitrate index" << endl;
        }

        switch (layerIndex) {
            case 0x01: {
                if (versionID == 0x11) bitrate = v1L3[bitrateIndex];
                else bitrate = v2All[bitrateIndex];
                break;
            }
            case 0x10: {
                if (versionID == 0x11) bitrate = v1L2[bitrateIndex];
                else bitrate = v2All[bitrateIndex];
            }
            case 0x11: {
                if (versionID == 0x11) bitrate = v1L1[bitrateIndex];
                else bitrate = v2L1[bitrateIndex];
            }
            default: cout << "decodeBitrate: Unknown layer index " << layerIndex << endl;
        }
    }

    void decodeSampleRate(uint8_t sampleRateIndex, const uint8_t versionID) {
        static const int v1[4] = {44100, 48000, 32000, 0};
        static const int v2[4] = {22050, 24000, 16000 ,0};
        static const int v2_5[4] = {11025, 12000, 8000, 0};

        if (sampleRateIndex > 4) cout << "Invalid bitrate index " << sampleRateIndex << endl;

        switch (versionID) {
            case 0x00: sampleRate = v2_5[sampleRateIndex]; break;
            case 0x10: sampleRate = v2[sampleRateIndex]; break;
            case 0x11: sampleRate = v1[sampleRateIndex]; break;
            default: cout << "decodeSampleRate: Invalid MPEG version." << endl;
        }
    }



    int length() const {
        return 0;
    }


};

int main() {
    // constexpr unsigned int input = 0b1111111111110101010101010000111;
    // Header const header(input);input
}