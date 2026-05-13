#ifndef RP_HEADER_HPP
#define RP_HEADER_HPP

#include <cstdint>
#include <string>

class Header {
public:
    explicit Header(unsigned int input, int &output);

    uint32_t getRaw() const;
    std::string getMpegVersion() const;
    std::string getLayer() const;
    bool hasCRC() const;
    int getBitrate() const;
    int getSampleRate() const;
    bool hasPadding() const;
    std::string getChannelMode() const;
    int getFrameLength() const;

    uint8_t getVersionID() const;
    uint8_t getLayerID() const;
    uint8_t getSampleRateIdx() const;
    uint8_t getChannelIdx() const;
    bool isMono() const;
    bool isLayerIII() const;

private:
    uint32_t raw;
    std::string mpegVersion;
    std::string layer;
    uint8_t protectionBit{};
    int bitrate{};
    int sampleRate{};
    bool padding{};
    std::string channelMode;
    int frameLength{};

    uint8_t versionID_raw{};
    uint8_t layerID_raw{};
    uint8_t sampleRateIdx_raw{};
    uint8_t channelIdx_raw{};

    bool synchValid() const;

    int decode();
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