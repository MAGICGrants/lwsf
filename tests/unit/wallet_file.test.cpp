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

#include "lws_frontend.h"

namespace
{
  std::string read_all(const std::string& path)
  {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  }

  void write_all(const std::string& path, const std::string& contents)
  {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << contents;
  }

  struct temp_dir
  {
    boost::filesystem::path path;
    temp_dir()
      : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("lwsf-wallet-%%%%-%%%%"))
    { boost::filesystem::create_directories(path); }
    ~temp_dir()
    {
      boost::system::error_code ignored;
      boost::filesystem::remove_all(path, ignored);
    }
  };

#if !defined(_WIN32)
  //! Runs `f` with files capped at `bytes`, the way a nearly full disk behaves.
  template<typename F>
  void with_file_size_limit(const rlim_t bytes, F&& f)
  {
    rlimit original{};
    getrlimit(RLIMIT_FSIZE, &original);
    const auto previous = std::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = original;
    limited.rlim_cur = bytes;
    setrlimit(RLIMIT_FSIZE, &limited);
    f();
    setrlimit(RLIMIT_FSIZE, &original);
    std::signal(SIGXFSZ, previous);
  }
#endif
}

LWS_CASE("wallet file survives unfinished saves")
{
  SETUP("a stored wallet")
  {
    const temp_dir dir{};
    const std::string path = (dir.path / "wallet").string();
    const std::string pending = path + ".new";
    // One manager for the whole run, as an app keeps one. getWalletManager
    // returns a new one on every call, and Monero::WalletManager has no
    // virtual destructor, so it cannot be deleted through that pointer.
    static Monero::WalletManager* const manager = lwsf::WalletManagerFactory::getWalletManager();

    {
      Monero::Wallet* const created =
        manager->createWallet(path, "password", "English", Monero::MAINNET, 1);
      EXPECT(created->status() == Monero::Wallet::Status_Ok);
      EXPECT(created->store({}));
      EXPECT(manager->closeWallet(created, false));
    }
    const std::string saved = read_all(path);
    EXPECT(64u < saved.size());

    const auto open = [&] (const std::string& password)
    {
      return manager->openWallet(path, password, Monero::MAINNET, 1, nullptr);
    };
    const auto opens = [&] (const std::string& password)
    {
      Monero::Wallet* const wallet = open(password);
      const bool ok = wallet->status() == Monero::Wallet::Status_Ok;
      manager->closeWallet(wallet, false);
      return ok;
    };

    SECTION("a .new cut short is passed over on open")
    {
      // It has the file magic, so it used to be loaded, fail to decrypt, and
      // stop the open, with a good wallet file right next to it.
      write_all(pending, saved.substr(0, saved.size() / 2));
      EXPECT(opens("password"));
      EXPECT(!opens("wrong password"));
    }

    SECTION("a .new cut short is deleted, not promoted, by the next save")
    {
      write_all(pending, saved.substr(0, saved.size() / 2));
      Monero::Wallet* const wallet = open("password");
      EXPECT(wallet->status() == Monero::Wallet::Status_Ok);
      EXPECT(wallet->store({}));
      manager->closeWallet(wallet, false);

      EXPECT(!boost::filesystem::exists(pending));
      EXPECT(opens("password"));
    }

#if !defined(_WIN32)
    SECTION("two saves on a full disk leave the wallet file as it was")
    {
      // The first used to rename a truncated file over the wallet; failing
      // ones used to leave a .new for the next save to rename over it.
      Monero::Wallet* const wallet = open("password");
      EXPECT(wallet->status() == Monero::Wallet::Status_Ok);
      with_file_size_limit(saved.size() / 2, [&] {
        EXPECT(!wallet->store({}));
        EXPECT(!wallet->store({}));
      });
      manager->closeWallet(wallet, false);

      EXPECT(read_all(path) == saved);
      EXPECT(!boost::filesystem::exists(pending));
      EXPECT(opens("password"));
    }

    SECTION("a complete .new is opened and then kept when the next save fails")
    {
      // A save whose rename never happened is the newest copy there is.
      write_all(pending, saved);
      write_all(path, saved.substr(0, 16));
      Monero::Wallet* const wallet = open("password");
      EXPECT(wallet->status() == Monero::Wallet::Status_Ok);
      with_file_size_limit(saved.size() / 2, [&] {
        EXPECT(!wallet->store({}));
      });
      manager->closeWallet(wallet, false);

      EXPECT(read_all(path) == saved);
      EXPECT(opens("password"));
    }
#endif
  }
}
