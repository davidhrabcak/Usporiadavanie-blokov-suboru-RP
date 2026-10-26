#ifndef RP_HEADER_HPP
#define RP_HEADER_HPP

#include <cstdint>
#include <string>

class Header {
public:
    explicit Header(unsigned int input, bool &ok);

    int getBitrate() const;
    int getSampleRate() const;
    bool hasPadding() const;
    int getFrameLength() const;

    uint8_t getVersionID() const;
    uint8_t getLayerID() const;
    uint8_t getSampleRateIdx() const;
    uint8_t getChannelIdx() const;
    bool isMono() const;
    bool isLayerIII() const;
    // protection_bit == 0: a 16-bit CRC follows the header, before the side info.
    bool hasCrc() const;
    int crcBytes() const { return hasCrc() ? 2 : 0; }

private:
    uint32_t raw;
    int bitrate{};
    int sampleRate{};
    bool padding{};
    int frameLength{};

    uint8_t versionID_raw{};
    uint8_t layerID_raw{};
    uint8_t sampleRateIdx_raw{};
    uint8_t channelIdx_raw{};

    bool synchValid() const;

    bool decode();
    bool decodeVersion(uint8_t versionID);
    bool decodeLayer(uint8_t layerIndex);
    bool decodeBitrate(uint8_t bitrateIndex,
                       uint8_t versionID,
                       uint8_t layerIndex);
    bool decodeSampleRate(uint8_t sampleRateIndex,
                          uint8_t versionID);
    bool decodeChannelMode(uint8_t channelIndex);

    int length(uint8_t versionID) const;
};

#endif