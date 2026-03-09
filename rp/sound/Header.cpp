#include "Header.hpp"

#include <iostream>

using namespace std;

Header::Header(unsigned int input) {
    raw = input;
    if (synchValid()) {
        decode();
    } else {
        cout << "Invalid input" << endl;
    }
}

uint32_t Header::getRaw() const { return raw; }
string Header::getMpegVersion() const { return mpegVersion; }
string Header::getLayer() const { return layer; }
bool Header::hasCRC() const { return protectionBit == 0; }
int Header::getBitrate() const { return bitrate; }
int Header::getSampleRate() const { return sampleRate; }
bool Header::hasPadding() const { return padding; }
string Header::getChannelMode() const { return channelMode; }
int Header::getFrameLength() const { return frameLength; }

bool Header::synchValid() const {
    return ((raw >> 21) & 0x7FF) == 0x7FF;
}

void Header::decode() {
    uint8_t versionID = (raw >> 19) & 0x3;
    uint8_t layerIndex = (raw >> 17) & 0x3;
    protectionBit = (raw >> 16) & 0x1;
    uint8_t bitrateIndex = (raw >> 12) & 0xF;
    uint8_t sampleIndex = (raw >> 10) & 0x3;
    padding = ((raw >> 9) & 0x1);
    uint8_t channelIndex = (raw >> 6) & 0x3;

    decodeVersion(versionID);
    decodeLayer(layerIndex);
    decodeBitrate(bitrateIndex, versionID, layerIndex);
    decodeSampleRate(sampleIndex, versionID);
    decodeChannelMode(channelIndex);

    frameLength = length(padding, protectionBit, layerIndex, versionID);
}

void Header::decodeVersion(uint8_t versionID) {
    switch (versionID) {
        case 0b00: mpegVersion = "MPEG 2.5"; break;
        case 0b10: mpegVersion = "MPEG 2"; break;
        case 0b11: mpegVersion = "MPEG 1"; break;
        default: mpegVersion = "unknown versionID " + to_string(versionID); break;
    }
}

void Header::decodeLayer(uint8_t layerIndex) {
    switch (layerIndex) {
        case 0b01: layer = "Layer III"; break;
        case 0b10: layer = "Layer II"; break;
        case 0b11: layer = "Layer I"; break;
        default: layer = "unknown layerIndex " + to_string(layerIndex); break;
    }
}

void Header::decodeBitrate(uint8_t bitrateIndex,
                           uint8_t versionID,
                           uint8_t layerIndex) {

    static const int v1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224,
                                256, 288, 320, 352, 384, 416, 448, -1};

    static const int v1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                128, 160, 192, 224, 256, 320, 384, -1};

    static const int v1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                112, 128, 160, 192, 224, 256, 320, -1};

    static const int v2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                128, 144, 160, 176, 192, 224, 256, -1};

    static const int v2All[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                 64, 80, 96, 112, 128, 144, 160, -1};

    if (bitrateIndex > 15) {
        cout << "Invalid bitrate index" << endl;
        return;
    }

    switch (layerIndex) {
        case 0b01:
            if (versionID == 0b11) bitrate = v1L3[bitrateIndex];
            else bitrate = v2All[bitrateIndex];
            break;

        case 0b10:
            if (versionID == 0b11) bitrate = v1L2[bitrateIndex];
            else bitrate = v2All[bitrateIndex];
            break;

        case 0b11:
            if (versionID == 0b11) bitrate = v1L1[bitrateIndex];
            else bitrate = v2L1[bitrateIndex];
            break;

        default:
            cout << "decodeBitrate: Unknown layer index " << int(layerIndex) << endl;
    }
}

void Header::decodeSampleRate(uint8_t sampleRateIndex,
                              uint8_t versionID) {

    static const int v1[4] = {44100, 48000, 32000, 0};
    static const int v2[4] = {22050, 24000, 16000 ,0};
    static const int v2_5[4] = {11025, 12000, 8000, 0};

    if (sampleRateIndex > 3) {
        cout << "Invalid sample rate index " << int(sampleRateIndex) << endl;
        return;
    }

    switch (versionID) {
        case 0b00: sampleRate = v2_5[sampleRateIndex]; break;
        case 0b10: sampleRate = v2[sampleRateIndex]; break;
        case 0b11: sampleRate = v1[sampleRateIndex]; break;
        default: cout << "decodeSampleRate: Invalid MPEG version." << endl;
    }
}

void Header::decodeChannelMode(uint8_t channelIndex) {
    switch (channelIndex) {
        case 0b00: channelMode = "Stereo"; break;
        case 0b01: channelMode = "Joint stereo"; break;
        case 0b10: channelMode = "Dual channel"; break;
        case 0b11: channelMode = "Mono"; break;
        default:
            cout << "decodeChannelMode: Invalid channel index "
                 << int(channelIndex) << endl;
    }
}

int Header::length(bool padding,
                   bool protectionBit,
                   uint8_t layerIndex,
                   uint8_t versionID) const {

    uint8_t crcLength = protectionBit ? 0 : 16;

    switch (versionID) {
        case 0b11: {
            uint8_t paddingLength = padding ? 32 : 0;
            return (144 * bitrate / (sampleRate / 1000.0))
                   + paddingLength + crcLength;
        }

        case 0b10:
        case 0b00: {
            uint8_t paddingLength = padding ? 8 : 0;
            return (144 * bitrate / (sampleRate / 1000.0))
                   + paddingLength + crcLength;
        }

        default:
            cout << "length: Invalid MPEG version." << endl;
            return -1;
    }
}