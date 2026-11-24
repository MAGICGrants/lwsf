// Copyright (c) 2025, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "file.h"

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <unistd.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

#include <algorithm>
#include <cstdint>

namespace lwsf { namespace internal
{
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
  bool atomic_file_write(const boost::filesystem::path& filename, const boost::filesystem::path& directory, epee::byte_slice contents) noexcept
  {
    const int fd =
      open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0)
      return false;

    // `write` may store fewer bytes than asked, and on a full disk it does: it
    // writes what fits and returns that count, not an error.
    bool rc = true;
    const std::uint8_t* next = contents.data();
    std::size_t remaining = contents.size();
    while (remaining)
    {
      const ssize_t written = write(fd, next, remaining);
      if (written < 0)
      {
        if (errno == EINTR)
          continue;
        rc = false;
        break;
      }
      if (written == 0)
      {
        rc = false; // no progress, and no error to say why
        break;
      }
      next += written;
      remaining -= std::size_t(written);
    }

    rc = rc && fsync(fd) == 0;
    rc = close(fd) == 0 && rc;

    if (rc)
    {
      const char* cwd = directory.c_str();
      if (std::strcmp(cwd, "") == 0)
        cwd = ".";
      const int dir = open(cwd, O_RDONLY);
      rc = 0 <= dir && fsync(dir) == 0;
      if (0 <= dir)
        close(dir);
    }

    if (!rc)
      unlink(filename.c_str());
    return rc;
  }
#elif defined(_WIN32)
  bool atomic_file_write(const boost::filesystem::path& filename, const boost::filesystem::path&, epee::byte_slice contents) noexcept
  {
    HANDLE file = CreateFileW(filename.c_str(), (GENERIC_READ | GENERIC_WRITE), 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return false;

    // `WriteFile` reports how much it wrote; a synchronous handle must be
    // given somewhere to put that count, and anything short is a failure.
    bool rc = true;
    const std::uint8_t* next = contents.data();
    std::size_t remaining = contents.size();
    while (rc && remaining)
    {
      const DWORD chunk = DWORD(std::min<std::size_t>(remaining, std::size_t(1) << 30));
      DWORD written = 0;
      rc = WriteFile(file, next, chunk, &written, nullptr) && written != 0;
      next += written;
      remaining -= written;
    }

    rc = rc && FlushFileBuffers(file);
    rc = CloseHandle(file) && rc;

    if (!rc)
      DeleteFileW(filename.c_str());
    return rc;
  }
#else
  #error atomic_file_write not implemented for this platform
#endif
}} // lwsf // internal
