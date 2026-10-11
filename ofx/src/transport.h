// Frame exchange with the worker through files in /dev/shm (RAM-backed).
// Layout must match worker/ltxag_worker/transport.py.
// Files are written and read with write()/pread(), never mmap: a full tmpfs then
// fails with ENOSPC instead of raising SIGBUS inside the Flame process.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ltxag {

constexpr uint32_t kTransportVersion = 1;

#pragma pack(push, 1)
struct FrameFileHeader {
  char magic[4];  // "LTXA" input frames, "LTXM" output mattes
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t frames;
  uint32_t channels;
};
#pragma pack(pop)
static_assert(sizeof(FrameFileHeader) == 24, "header must be 24 bytes");

// A uniquely named file in /dev/shm, unlinked when this object dies.
class ShmFile {
 public:
  explicit ShmFile(const std::string& tag);
  ~ShmFile();
  ShmFile(const ShmFile&) = delete;
  ShmFile& operator=(const ShmFile&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Sequential writer for the input file: header, then one frame at a time.
class FrameWriter {
 public:
  // Reserves the full size up front (posix_fallocate) so lack of RAM shows up here.
  bool open(const std::string& path, uint32_t w, uint32_t h, uint32_t n, std::string& err);
  bool writeFrame(const uint16_t* halfRGB, std::string& err);  // w*h*3 halves, rows top-down
  void close();
  ~FrameWriter() { close(); }

 private:
  int fd_ = -1;
  size_t frameBytes_ = 0;
};

// Reads the worker's output into halves (n*h*w), checking the header.
bool readMattes(const std::string& path, uint32_t w, uint32_t h, uint32_t n,
                std::vector<uint16_t>& out, std::string& err);

}  // namespace ltxag
