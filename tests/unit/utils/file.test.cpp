// Copyright (c) 2025, The Monero Project
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


#include "framework.test.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <string>

#if !defined(_WIN32)
  #include <csignal>
  #include <sys/resource.h>
#endif

#include "byte_slice.h" // monero/contrib/epee/include
#include "utils/file.h"

namespace
{
  std::string pattern(const std::size_t size)
  {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
      out[i] = char('a' + i % 26);
    return out;
  }

  std::string read_all(const boost::filesystem::path& path)
  {
    std::ifstream in{path.string(), std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  }

  struct temp_dir
  {
    boost::filesystem::path path;
    temp_dir()
      : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("lwsf-file-%%%%-%%%%"))
    { boost::filesystem::create_directories(path); }
    ~temp_dir()
    {
      boost::system::error_code ignored;
      boost::filesystem::remove_all(path, ignored);
    }
  };
}

LWS_CASE("utils::atomic_file_write")
{
  SETUP("a new file in an empty directory")
  {
    const temp_dir dir{};
    const boost::filesystem::path file = dir.path / "wallet.new";

    SECTION("writes every byte")
    {
      const std::string contents = pattern(100000);
      EXPECT(lwsf::internal::atomic_file_write(file, dir.path, epee::byte_slice{std::string{contents}}));
      EXPECT(read_all(file) == contents);
    }

    SECTION("refuses to replace a file that is already there")
    {
      { std::ofstream{file.string(), std::ios::binary} << "keep"; }
      EXPECT(!lwsf::internal::atomic_file_write(file, dir.path, epee::byte_slice{pattern(64)}));
      EXPECT(read_all(file) == "keep");
    }

#if !defined(_WIN32)
    SECTION("a short write fails and leaves no file behind")
    {
      // A file-size limit makes `write` store only what fits and return that
      // count, as a full disk does. That used to be reported as success, and
      // the truncated file was then renamed over the wallet.
      rlimit original{};
      EXPECT(getrlimit(RLIMIT_FSIZE, &original) == 0);
      const auto previous = std::signal(SIGXFSZ, SIG_IGN);
      rlimit limited = original;
      limited.rlim_cur = 4096;
      EXPECT(setrlimit(RLIMIT_FSIZE, &limited) == 0);

      const bool written =
        lwsf::internal::atomic_file_write(file, dir.path, epee::byte_slice{pattern(100000)});

      setrlimit(RLIMIT_FSIZE, &original);
      std::signal(SIGXFSZ, previous);

      EXPECT(!written);
      EXPECT(!boost::filesystem::exists(file));
    }
#endif
  }
}
