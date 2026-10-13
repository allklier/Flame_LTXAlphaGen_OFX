#include "transport.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#include "platform.h"

namespace ltxag {

namespace {
std::atomic<unsigned> gSeq{0};

// Reserves `total` bytes so lack of space fails here, not mid-write. Returns 0 or an errno.
// macOS has no posix_fallocate (not in the MacOSX.sdk fcntl.h): F_PREALLOCATE with
// F_ALLOCATEALL (all or nothing, sys/fcntl.h) from the end of file, then ftruncate to the
// same size posix_fallocate leaves.
int reserve(int fd, off_t total) {
#if defined(__APPLE__)
  fstore_t fs{F_ALLOCATEALL, F_PEOFPOSMODE, 0, total, 0};
  if (fcntl(fd, F_PREALLOCATE, &fs) != 0 || ftruncate(fd, total) != 0) return errno;
  return 0;
#else
  return ::posix_fallocate(fd, 0, total);
#endif
}

bool writeAll(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    const ssize_t k = ::write(fd, p, len);
    if (k < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += k;
    len -= size_t(k);
  }
  return true;
}
}  // namespace

StagingFile::StagingFile(const std::string& tag) {
  path_ = stagingDir() + "/ltxag_" + std::to_string(getpid()) + "_" + std::to_string(gSeq++) + "_" +
          tag + ".bin";
}

StagingFile::~StagingFile() { ::unlink(path_.c_str()); }

bool FrameWriter::open(const std::string& path, uint32_t w, uint32_t h, uint32_t n,
                       std::string& err) {
  close();
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    err = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }
  frameBytes_ = size_t(w) * h * 3 * sizeof(uint16_t);
  const off_t total = off_t(sizeof(FrameFileHeader)) + off_t(frameBytes_) * n;
  const int rc = reserve(fd_, total);
  if (rc != 0) {
    err = "not enough space in " + stagingDir() + " for " + std::to_string(total >> 20) +
          " MB of frames: " + std::strerror(rc);
    return false;
  }
  FrameFileHeader hdr{{'L', 'T', 'X', 'A'}, kTransportVersion, w, h, n, 3};
  if (!writeAll(fd_, &hdr, sizeof hdr)) {
    err = std::string("header write failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool FrameWriter::writeFrame(const uint16_t* halfRGB, std::string& err) {
  if (!writeAll(fd_, halfRGB, frameBytes_)) {
    err = std::string("frame write failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

void FrameWriter::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool readMattes(const std::string& path, uint32_t w, uint32_t h, uint32_t n,
                std::vector<uint16_t>& out, std::string& err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    err = "worker produced no output file";
    return false;
  }
  FrameFileHeader hdr{};
  bool ok = ::pread(fd, &hdr, sizeof hdr, 0) == ssize_t(sizeof hdr) &&
            std::memcmp(hdr.magic, "LTXM", 4) == 0 && hdr.version == kTransportVersion &&
            hdr.width == w && hdr.height == h && hdr.frames == n && hdr.channels == 1;
  if (!ok) {
    err = "worker output header does not match the request";
  } else {
    out.resize(size_t(w) * h * n);
    const size_t bytes = out.size() * sizeof(uint16_t);
    size_t done = 0;
    while (done < bytes) {
      const ssize_t k = ::pread(fd, reinterpret_cast<char*>(out.data()) + done, bytes - done,
                                off_t(sizeof hdr + done));
      if (k <= 0) {
        if (k < 0 && errno == EINTR) continue;
        break;
      }
      done += size_t(k);
    }
    if (done != bytes) {
      ok = false;
      err = "worker output is truncated";
    }
  }
  ::close(fd);
  return ok;
}

}  // namespace ltxag
