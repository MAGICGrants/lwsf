// Copyright (c) 2026, The Monero Project
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

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>
#include "byte_slice.h"  // monero/contrib/epee/include
#include "net/http.h"
#include "net/net_ssl.h" // monero/contrib/epee/include

namespace
{
  // Throwaway self-signed certificate for localhost / 127.0.0.1, valid until
  // 2126, with its private key. It serves as both the client's only trust
  // anchor and the server's certificate. It protects nothing.
  constexpr const char cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBlDCCATqgAwIBAgIJAL4NME01flOsMAoGCCqGSM49BAMCMBQxEjAQBgNVBAMM\n"
    "CWxvY2FsaG9zdDAgFw0yNjEwMDMxNDUwMzVaGA8yMTI2MDkwOTE0NTAzNVowFDES\n"
    "MBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEiZON\n"
    "oMSLiAQKromTonQ+Fkgn7dbllXihNIz6+VKNuneojAMXZ0nHqu58V6RtopOR2JlV\n"
    "E8nX4DxqYWrnC5uBlKNzMHEwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
    "AoQwEwYDVR0lBAwwCgYIKwYBBQUHAwEwGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/\n"
    "AAABMB0GA1UdDgQWBBQH6DhXQwo0q6xYn701K5B3EYBfwzAKBggqhkjOPQQDAgNI\n"
    "ADBFAiEA2jvc5z1w8xsAb+QfUTHLTJ4AMyUTuXc3q3FYnn66uoACIFugSG97n88f\n"
    "OHCHYSN+MIEdUjxFAHuRyzOgCG7ZiJqX\n"
    "-----END CERTIFICATE-----\n";

  constexpr const char key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgMCPxGNIBBYBjN2sQ\n"
    "Fn1ZryjwoeLsp2J8EjR0135BOAihRANCAASJk42gxIuIBAquiZOidD4WSCft1uWV\n"
    "eKE0jPr5Uo26d6iMAxdnSceq7nxXpG2ik5HYmVUTydfgPGphaucLm4GU\n"
    "-----END PRIVATE KEY-----\n";

  using tcp = boost::asio::ip::tcp;
  using tls_stream = boost::asio::ssl::stream<tcp::socket>;

  template<typename F>
  void run_until(boost::asio::io_context& io, F done)
  {
    while (!done())
    {
      io.restart();
      io.run_one();
    }
  }

  //! Writes `cert_pem` to a fresh file under the temp directory; removed on destruction.
  struct temp_ca_file
  {
    std::string path;

    temp_ca_file()
      : path()
    {
      std::random_device rng{};
      path = (boost::filesystem::temp_directory_path() /
        ("lwsf-tls-test-" + std::to_string(rng()) + "-" + std::to_string(rng()) + ".pem")).string();
      std::ofstream out{path, std::ios::binary};
      out << cert_pem;
    }

    ~temp_ca_file()
    {
      std::remove(path.c_str());
    }
  };

  epee::net_utils::ssl_options_t user_ca(std::string path)
  {
    epee::net_utils::ssl_options_t options{std::vector<std::vector<std::uint8_t>>{}, std::move(path)};
    options.verification = epee::net_utils::ssl_verification_t::user_ca;
    return options;
  }

  /*! Accepts one TLS connection, answers its first request with `body` and
    `Connection: close`, then closes the socket the way an LWS behind a proxy
    does when a keep-alive ends. \return The request target, or empty when the
    TLS handshake failed. */
  std::string serve_once(boost::asio::io_context& io, tcp::acceptor& acceptor, boost::asio::ssl::context& ctx, const std::string& body)
  {
    tls_stream server{io, ctx};
    bool accepted = false;
    acceptor.async_accept(server.next_layer(), [&] (boost::system::error_code) { accepted = true; });
    run_until(io, [&] { return accepted; });

    bool handshake_done = false;
    boost::system::error_code handshake_error{};
    server.async_handshake(
      boost::asio::ssl::stream_base::server,
      [&] (const boost::system::error_code error) { handshake_error = error; handshake_done = true; }
    );
    run_until(io, [&] { return handshake_done; });
    if (handshake_error)
      return {};

    boost::beast::flat_buffer buffer{};
    boost::beast::http::request<boost::beast::http::string_body> request{};
    bool read_done = false;
    boost::beast::http::async_read(server, buffer, request, [&] (boost::system::error_code, std::size_t) { read_done = true; });
    run_until(io, [&] { return read_done; });

    boost::beast::http::response<boost::beast::http::string_body> response{};
    response.keep_alive(false);
    response.body() = body;
    response.prepare_payload();
    bool write_done = false;
    boost::beast::http::async_write(server, response, [&] (boost::system::error_code, std::size_t) { write_done = true; });
    run_until(io, [&] { return write_done; });

    boost::system::error_code ignore{};
    server.next_layer().shutdown(tcp::socket::shutdown_both, ignore);
    server.next_layer().close(ignore);
    return std::string(request.target().data(), request.target().size());
  }

  std::string to_string(const epee::byte_slice& src)
  {
    return {reinterpret_cast<const char*>(src.data()), src.size()};
  }
}

LWS_CASE("net::http tls")
{
  SETUP("client+tls server")
  {
    boost::asio::io_context io{};
    tcp::acceptor acceptor{io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)};
    acceptor.listen();

    boost::asio::ssl::context server_ctx{boost::asio::ssl::context::tls_server};
    server_ctx.use_certificate_chain(boost::asio::buffer(cert_pem, sizeof(cert_pem) - 1));
    server_ctx.use_private_key(boost::asio::buffer(key_pem, sizeof(key_pem) - 1), boost::asio::ssl::context::pem);

    const temp_ca_file ca{};
    lwsf::internal::http::client client{};

    SECTION("a new TLS session after the server closes the connection")
    {
      client.init(io, "127.0.0.1", "", acceptor.local_endpoint().port(), user_ca(ca.path));

      std::error_code first_error = std::make_error_code(std::errc::operation_canceled);
      epee::byte_slice first;
      bool first_done = false;
      client.get_async("/first", [&] (std::error_code error, epee::byte_slice body) {
        first_error = error;
        first = std::move(body);
        first_done = true;
      });
      EXPECT(serve_once(io, acceptor, server_ctx, "ONE") == "/first");
      run_until(io, [&] { return first_done; });
      EXPECT(!first_error);
      EXPECT(to_string(first) == "ONE");

      // The server closed the first connection, so this request needs a new
      // TCP connection and a new TLS handshake.
      std::error_code second_error = std::make_error_code(std::errc::operation_canceled);
      epee::byte_slice second;
      bool second_done = false;
      client.get_async("/second", [&] (std::error_code error, epee::byte_slice body) {
        second_error = error;
        second = std::move(body);
        second_done = true;
      });
      EXPECT(serve_once(io, acceptor, server_ctx, "TWO") == "/second");
      run_until(io, [&] { return second_done; });
      EXPECT(!second_error);
      EXPECT(to_string(second) == "TWO");
    }

    SECTION("an unreadable CA file is an error, not a fallback")
    {
      EXPECT_THROWS(client.init(io, "127.0.0.1", "", acceptor.local_endpoint().port(), user_ca(ca.path + ".missing")));
    }
  } // SETUP
}
