#include "Header.hpp"

#include <iostream>

using namespace std;

Header::Header(const unsigned int input, bool &ok) {
    raw = input;
    if (synchValid()) {
        ok = decode();
    } else {
        ok = false;
        cerr << "Invalid input" << endl;
    }
}

int Header::getBitrate() const { return bitrate; }
int Header::getSampleRate() const { return sampleRate; }
bool Header::hasPadding() const { return padding; }
int Header::getFrameLength() const { return frameLength; }

bool Header::synchValid() const {
    return ((raw >> 21) & 0x7FF) == 0x7FF;
}

bool Header::decode() {
    const uint8_t versionID = (raw >> 19) & 0x3;
    const uint8_t layerIndex = (raw >> 17) & 0x3;
    const uint8_t bitrateIndex = (raw >> 12) & 0xF;
    const uint8_t sampleIndex = (raw >> 10) & 0x3;
    padding = ((raw >> 9) & 0x1);
    const uint8_t channelIndex = (raw >> 6) & 0x3;

    versionID_raw     = versionID;
    layerID_raw       = layerIndex;
    sampleRateIdx_raw = sampleIndex;
    channelIdx_raw    = channelIndex;

    if (!decodeVersion(versionID)) return false;
    if (!decodeLayer(layerIndex)) return false;
    if (!decodeBitrate(bitrateIndex, versionID, layerIndex)) return false;
    if (!decodeSampleRate(sampleIndex, versionID)) return false;
    if (!decodeChannelMode(channelIndex)) return false;

    frameLength = length(versionID);
    return true;
}

// versionID 01 is a reserved MPEG version - the only invalid value of this 2-bit field.
bool Header::decodeVersion(const uint8_t versionID) {
    return versionID == 0b00 || versionID == 0b10 || versionID == 0b11;
}

// layerIndex 00 is reserved (no such layer) - the only invalid value of this 2-bit field.
bool Header::decodeLayer(const uint8_t layerIndex) {
    return layerIndex == 0b01 || layerIndex == 0b10 || layerIndex == 0b11;
}

bool Header::decodeBitrate(const uint8_t bitrateIndex,
                           const uint8_t versionID,
                           const uint8_t layerIndex) {

    static constexpr int v1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224,
                                256, 288, 320, 352, 384, 416, 448, -1};

    static constexpr int v1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                128, 160, 192, 224, 256, 320, 384, -1};

    static constexpr int v1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                112, 128, 160, 192, 224, 256, 320, -1};

    static constexpr int v2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                                128, 144, 160, 176, 192, 224, 256, -1};

    static constexpr int v2All[16] = {0, 8, 16, 24, 32, 40, 48, 56,
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
            cerr << "decodeBitrate: Unknown layer index " << static_cast<int>(layerIndex) << endl;
            return false;
        }
    }
}

bool Header::decodeSampleRate(const uint8_t sampleRateIndex,
                              const uint8_t versionID) {

    static constexpr int v1[4] = {44100, 48000, 32000, 0};
    static constexpr int v2[4] = {22050, 24000, 16000 ,0};
    static constexpr int v2_5[4] = {11025, 12000, 8000, 0};

    int sr;

    switch (versionID) {
        case 0b00: sr = v2_5[sampleRateIndex]; break;
        case 0b10: sr = v2[sampleRateIndex]; break;
        case 0b11: sr = v1[sampleRateIndex]; break;
        default: return false;
    }

    if (sr == 0) return false;

    sampleRate = sr;
    return true;
}

// channelIndex is a 2-bit field; all four values (stereo/joint/dual/mono) are valid.
bool Header::decodeChannelMode(const uint8_t) {
    return true;
}

int Header::length(const uint8_t versionID) const {
    if (sampleRate <= 0 || bitrate <= 0) return -1;

    const int coeff = (versionID == 0b11) ? 144 : 72;

    int frameLen = (coeff * bitrate * 1000) / sampleRate;

    if (padding) frameLen += 1;

    return frameLen;
}

uint8_t Header::getVersionID() const    { return versionID_raw; }
uint8_t Header::getLayerID() const      { return layerID_raw; }
uint8_t Header::getSampleRateIdx() const{ return sampleRateIdx_raw; }
uint8_t Header::getChannelIdx() const   { return channelIdx_raw; }
bool    Header::isMono() const          { return channelIdx_raw == 0b11; }
bool    Header::isLayerIII() const      { return layerID_raw == 0b01; }