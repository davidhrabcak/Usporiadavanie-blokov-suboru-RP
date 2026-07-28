#ifndef RP_FRAME_SCANNER_HPP
#define RP_FRAME_SCANNER_HPP

#include <vector>
#include <ostream>
#include <cstdint>

struct FrameData {
    std::vector<uint8_t> rawBits;
};

struct FrameInfo {
    size_t position{};   // byte offset in file
    int length{};     // frame length in bytes
};

// Packs 4 consecutive bytes (MSB-first) into the 32-bit value an MP3 frame header decodes from.
inline uint32_t packHeaderBytes(const uint8_t* b) {
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16)
         | (static_cast<uint32_t>(b[2]) << 8)  |  static_cast<uint32_t>(b[3]);
}

class Mp3FrameScanner {
    public:
    Mp3FrameScanner(const std::string& filename);
    const std::vector<FrameInfo>& getFrames();
    unsigned long getFrameCount() const;
    static bool isValidHeader(uint32_t headerRaw);
    /** Returns each frame's raw bytes, in file order - one chunk per frame. */
    std::vector<std::vector<uint8_t>> getRawChunks() const;
    std::vector<FrameData> frame_data;

    private:

    std::vector<uint8_t> data;
    std::vector<FrameInfo> frames;



    void loadFile(const std::string& filename);
    size_t skipID3() const;
    void scanFrames();
};

#endif