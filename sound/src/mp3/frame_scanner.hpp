#ifndef RP_FRAME_SCANNER_HPP
#define RP_FRAME_SCANNER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FrameInfo {
    size_t position{};   // byte offset in file
    int length{};        // frame length in bytes
};

// Loads an MP3 file and finds its frames (a leading ID3v2 tag is skipped).
class Mp3FrameScanner {
public:
    explicit Mp3FrameScanner(const std::string& filename);
    const std::vector<FrameInfo>& getFrames() const { return frames; }
    size_t getFrameCount() const { return frames.size(); }
    /** The audio stream - from the first frame header to the end of the file, without the tags
     *  appended after the last frame (ID3v1, Lyrics3v2, APEv2; see audioEnd) - cut into
     *  `chunkSize`-byte chunks; the last one may be shorter. Empty if no frame was found. */
    std::vector<std::vector<uint8_t>> chunksFromFirstFrame(long chunkSize) const;

private:
    std::vector<uint8_t> data;
    std::vector<FrameInfo> frames;

    size_t skipID3() const;
    size_t audioEnd() const;
    void scanFrames();
};

#endif
