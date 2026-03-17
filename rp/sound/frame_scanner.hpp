#ifndef RP_FRAME_SCANNER_HPP
#define RP_FRAME_SCANNER_HPP

#include <vector>
#include <ostream>

struct FrameData {
    std::string mpeg_version;
    std::string layer;
    bool crc;
    int bitrate;
    int sample_rate;
    std::string channel_mode;
};

std::ostream& operator<<(std::ostream &os, const FrameData& data) {
    os << "MPEG: " << data.mpeg_version << std::endl;
    os << "Layer: " << data.layer << std::endl;
    os << "CRC: " << ((data.crc) ? "Used" : "Not used") << std::endl;
    os << "Bitrate: " << data.bitrate << std::endl;
    os << "Sample rate: " << data.sample_rate << std::endl;
    os << "Channel mode: " << data.channel_mode << std::endl;
    return os;
}

struct FrameInfo {
    size_t position;   // byte offset in file
    int length;     // frame length in bytes
    FrameData frame_data;
};


class Mp3FrameScanner {
    public:
    Mp3FrameScanner(const std::string& filename);
    const std::vector<FrameInfo>& getFrames();
    unsigned long getFrameCount();
    FrameData getFrame(size_t index);

    private:
    void loadFile(const std::string& filename);
    size_t skipID3() const;
    static bool isValidHeader(uint32_t headerRaw);
    void scanFrames();
};

#endif