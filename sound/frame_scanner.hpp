#ifndef RP_FRAME_SCANNER_HPP
#define RP_FRAME_SCANNER_HPP

#include <vector>
#include <ostream>
#include <cstdint>
#include <string>
#include "Header.hpp"

struct FrameData {
    std::vector<uint8_t> rawBits;
};

struct FrameInfo {
    size_t position{};   // byte offset in file
    int length{};     // frame length in bytes
};


class Mp3FrameScanner {
    public:
    Mp3FrameScanner(const std::string& filename);
    const std::vector<FrameInfo>& getFrames();
    unsigned long getFrameCount() const;
    /** The file from its first frame header on (leading tags skipped), cut into `chunkSize`-byte
     *  chunks; the last one may be shorter. Empty if no frame was found. */
    std::vector<std::vector<uint8_t>> chunksFromFirstFrame(long chunkSize) const;
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