#include "Header.hpp"

#include <iostream>

using namespace std;

Header::Header(const unsigned int input, int &output) {
    raw = input;
    if (synchValid()) {
        output = decode();
    } else {
        cerr << "Invalid input" << endl;
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

int Header::decode() {
    const uint8_t versionID = (raw >> 19) & 0x3;
    const uint8_t layerIndex = (raw >> 17) & 0x3;
    protectionBit = (raw >> 16) & 0x1;
    const uint8_t bitrateIndex = (raw >> 12) & 0xF;
    const uint8_t sampleIndex = (raw >> 10) & 0x3;
    padding = ((raw >> 9) & 0x1);
    const uint8_t channelIndex = (raw >> 6) & 0x3;

    if (decodeVersion(versionID)) return -1;
    if (decodeLayer(layerIndex)) return -2;
    if (decodeBitrate(bitrateIndex, versionID, layerIndex)) return -3;
    if (decodeSampleRate(sampleIndex, versionID)) return -4;
    if (decodeChannelMode(channelIndex)) return -5;

    frameLength = length(padding, protectionBit, versionID);
    return 0;
}

bool Header::decodeVersion(const uint8_t versionID) {
    switch (versionID) {
        case 0b00: mpegVersion = "MPEG 2.5"; return true;
        case 0b10: mpegVersion = "MPEG 2"; return true;
        case 0b11: mpegVersion = "MPEG 1"; return true;
        default: {
            mpegVersion = "unknown versionID " + to_string(versionID); return false;
        }
    }
}

bool Header::decodeLayer(const uint8_t layerIndex) {
    switch (layerIndex) {
        case 0b01: layer = "Layer III"; return true;
        case 0b10: layer = "Layer II"; return true;
        case 0b11: layer = "Layer I"; return true;
        default: {
            layer = "unknown layerIndex " + to_string(layerIndex); return false;
        }
    }
}

bool Header::decodeBitrate(const uint8_t bitrateIndex,
                           const uint8_t versionID,
                           const uint8_t layerIndex) {

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
        cerr << "Invalid bitrate index" << endl;
        return false;
    }

    switch (layerIndex) {
        case 0b01:
            if (versionID == 0b11) bitrate = v1L3[bitrateIndex];
            else bitrate = v2All[bitrateIndex];
            return true;

        case 0b10:
            if (versionID == 0b11) bitrate = v1L2[bitrateIndex];
            else bitrate = v2All[bitrateIndex];
            return true;

        case 0b11:
            if (versionID == 0b11) bitrate = v1L1[bitrateIndex];
            else bitrate = v2L1[bitrateIndex];
            return true;

        default: {
            cerr << "decodeBitrate: Unknown layer index " << int(layerIndex) << endl;
            return false;
        }
    }
}

bool Header::decodeSampleRate(uint8_t sampleRateIndex,
                              uint8_t versionID) {

    static const int v1[4] = {44100, 48000, 32000, 0};
    static const int v2[4] = {22050, 24000, 16000 ,0};
    static const int v2_5[4] = {11025, 12000, 8000, 0};

    if (sampleRateIndex > 3) {
        cerr << "Invalid sample rate index " << int(sampleRateIndex) << endl;
        return false;
    }

    switch (versionID) {
        case 0b00: sampleRate = v2_5[sampleRateIndex]; return true;
        case 0b10: sampleRate = v2[sampleRateIndex]; return true;
        case 0b11: sampleRate = v1[sampleRateIndex]; return true;
        default: {
            cerr << "decodeSampleRate: Invalid MPEG version." << endl;
            return false;
        }
    }
}

bool Header::decodeChannelMode(uint8_t channelIndex) {
    switch (channelIndex) {
        case 0b00: channelMode = "Stereo"; return true;
        case 0b01: channelMode = "Joint stereo"; return true;
        case 0b10: channelMode = "Dual channel"; return true;
        case 0b11: channelMode = "Mono"; return true;
        default: {
            cerr << "decodeChannelMode: Invalid channel index "
                 << int(channelIndex) << endl;
            return false;
        }
    }
}

int Header::length(bool padding,
                   bool protectionBit,
                   uint8_t versionID) const {

    const uint8_t crcLength = protectionBit ? 0 : 16;

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
            cerr << "length: Invalid MPEG version." << endl;
            return -1;
    }
}