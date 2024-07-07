/******************************************************************************
**
** Copyright (C) 2024 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the aserver - which can be found at
** https://github.com/IvanPinezhaninov/aserver/.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
** DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
** OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
** THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************/

#include <iostream>

#include <aserver.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/core/ignore_unused.hpp>

#include "config.h"

// To connect to the server, first generate the key and certificate
// using the script provided in the 'ssl' directory. Then use:
// socat STDIO OPENSSL:localhost:12345,verify=0

namespace net = boost::asio;
namespace ip = net::ip;
namespace ssl = net::ssl;
namespace sys = boost::system;

using namespace aserver;

class TlsEcho final : public server_worker<ip::tcp> {
public:
  explicit TlsEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {
    std::cout << "Worker created" << std::endl;
  }

  ~TlsEcho() { std::cout << "Worker destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop, net::yield_context yield) final
  {
    boost::ignore_unused(ioc);

    try {
      std::cout << "Client connected: " << socket.remote_endpoint() << std::endl;
      auto stream = tlsHandshake(std::move(socket), yield);

      sys::error_code ec;
      auto stopped = false;
      stop.connect([&] {
        net::dispatch(stream.get_executor(), [&] {
          stopped = true;
          stream.lowest_layer().close(ec);
        });
      });

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(stream, buffer, yield);

      while (!stopped) {
        sys::error_code ec;
        net::async_read_until(stream, buffer, "\n", yield[ec]);
        if (ec == net::error::eof) {
          std::cout << "Client disconnected: " << stream.lowest_layer().remote_endpoint() << std::endl;
          break;
        } else if (ec == net::error::operation_aborted && stopped) {
          std::cout << "Worker stopped" << std::endl;
          break;
        }
        if (ec)
          throw sys::system_error{ec};
        net::async_write(stream, buffer, yield);
      }
    } catch (const std::exception &e) {
      std::cerr << "Communication error: " << e.what() << std::endl;
    }
  }

private:
  ssl::stream<protocol_type::socket> tlsHandshake(protocol_type::socket socket, net::yield_context yield) const
  {
    std::array<char, 1> data;
    socket.async_receive(net::buffer(data), net::socket_base::message_peek, yield);
    static constexpr auto tlsHandshakeContentType = 0x16;
    if (static_cast<unsigned char>(data.front()) != tlsHandshakeContentType) {
      static constexpr std::string_view error = "TLS required. Please connect using a TLS-enabled client.\n";
      net::async_write(socket, net::buffer(error), yield);
      throw std::runtime_error{"TLS required"};
    }

    ssl::context ctx{ssl::context::tls_server};
    ctx.use_certificate_chain_file(std::string{sslDir}.append("/server.crt"));
    ctx.use_private_key_file(std::string{sslDir}.append("/server.key"), ssl::context::pem);
    ssl::stream<protocol_type::socket> stream{std::move(socket), ctx};
    stream.async_handshake(ssl::stream_base::server, yield);
    return stream;
  }

  std::string m_greetings;
};

int main(int, char **)
{
  try {
    server server{std::thread::hardware_concurrency()};
    auto stop = server.bind_endpoint<TlsEcho>({ip::tcp::v4(), 12345}, "Hello! I'm an echo server.");

    server.on_run([] { std::cout << "Server started" << std::endl; });
    server.on_stop([] { std::cout << "Server stopped" << std::endl; });

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&stop](const sys::error_code &ec) {
      if (!ec)
        stop();
    });

    net::signal_set signals{server.ioc(), SIGINT, SIGTERM};
    signals.async_wait(std::bind(&server::stop, &server));

    server.run();
    return EXIT_SUCCESS;
  } catch (const std::exception &e) {
    std::cerr << "Server run error: " << e.what() << std::endl;
  }

  return EXIT_FAILURE;
}
