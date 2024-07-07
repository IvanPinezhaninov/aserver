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

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/core/ignore_unused.hpp>

// To connect to the server use:
// socat STDIO TCP4:localhost:12345

namespace {

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

using namespace aserver;

void logError(const ip::tcp::endpoint &endpoint, std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const sys::system_error &e) {
    std::cout << "An error occurred on endpoint: " << endpoint << ". Error: " << e.code().message() << std::endl;
  } catch (...) {
    std::cout << "An unknown error occurred on endpoint: " << endpoint << std::endl;
  }
}

class TcpEcho final : public receiver<ip::tcp> {
public:
  explicit TcpEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {
    std::cout << "Receiver created" << std::endl;
  }

  ~TcpEcho() { std::cout << "Receiver destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop,
                  net::yield_context yield) override
  {
    boost::ignore_unused(ioc);
    stop_signal::connection_type conn;
    protocol_type::socket::endpoint_type endpoint;

    try {
      conn = stop.connect([&] { socket.cancel(); });

      endpoint = socket.remote_endpoint();
      std::cout << "Client connected: " << endpoint << std::endl;

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(socket, buffer, yield);

      while (true) {
        try {
          net::async_read_until(socket, buffer, "\n", yield);
          net::async_write(socket, buffer, yield);
        } catch (const sys::system_error &e) {
          if (e.code() == net::error::eof) {
            std::cout << "Client disconnected: " << endpoint << std::endl;
            break;
          } else if (e.code() == net::error::operation_aborted) {
            break;
          }
          throw;
        }
      }
    } catch (...) {
      logError(endpoint, std::current_exception());
    }

    socket.close();
    conn.disconnect();
    std::cout << "Receiver closed: " << endpoint << std::endl;
  }

private:
  std::string m_greetings;
};

} // namespace

int main(int, char **)
{
  try {
    server server{std::thread::hardware_concurrency()};
    server.on_run([] { std::cout << "Server started" << std::endl; });
    server.on_stop([] { std::cout << "Server stopped" << std::endl; });

    net::ip::tcp::endpoint endpoint{ip::tcp::v4(), 12345};
    auto stop =
        server.bind_receiver<TcpEcho>(endpoint, [&](auto e) { logError(endpoint, e); }, "Hello! I'm an echo server.");

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&](const sys::error_code &ec) {
      if (!ec) stop();
    });

    net::signal_set signals{server.ioc(), SIGINT, SIGTERM};
#ifdef _WIN32
    signals.add(SIGBREAK);
#endif
    signals.async_wait([&server](const sys::error_code &ec, int sig) {
      boost::ignore_unused(sig);
      if (!ec) server.stop();
    });

    server.run();
    return EXIT_SUCCESS;
  } catch (const sys::system_error &e) {
    std::cerr << "An server error occurred: " << e.code().message() << std::endl;
  } catch (...) {
    std::cerr << "An unknown server error occurred" << std::endl;
  }

  return EXIT_FAILURE;
}
