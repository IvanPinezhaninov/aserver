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

#include <aserver.hpp>
#include <boost/asio.hpp>
#include <boost/core/ignore_unused.hpp>

// To connect to the server use:
// socat STDIO TCP4:localhost:12345

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

using namespace aserver;

class TcpEcho final : public receiver<ip::tcp> {
public:
  explicit TcpEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {
    std::cout << "Receiver created" << std::endl;
  }

  ~TcpEcho() { std::cout << "Receiver destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop, net::yield_context yield) final
  {
    boost::ignore_unused(ioc);

    try {
      std::cout << "Client connected: " << socket.remote_endpoint() << std::endl;

      sys::error_code ec;
      auto stopped = false;
      stop.connect([&] {
        net::dispatch(socket.get_executor(), [&] {
          stopped = true;
          socket.close(ec);
        });
      });

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(socket, buffer, yield);

      while (!stopped) {
        net::async_read_until(socket, buffer, "\n", yield[ec]);
        if (ec == net::error::eof) {
          std::cout << "Client disconnected: " << socket.remote_endpoint() << std::endl;
          break;
        } else if (ec == net::error::operation_aborted && stopped) {
          std::cout << "Receiver stopped" << std::endl;
          break;
        }
        if (ec) throw sys::system_error{ec};
        net::async_write(socket, buffer, yield);
      }
    } catch (const std::exception &e) {
      std::cerr << "Communication error: " << e.what() << std::endl;
    }
  }

private:
  std::string m_greetings;
};

int main(int, char **)
{
  try {
    server server{std::thread::hardware_concurrency()};
    auto stop = server.bind_receiver<TcpEcho>({ip::tcp::v4(), 12345}, "Hello! I'm an echo server.");

    server.on_run([] { std::cout << "Server started" << std::endl; });
    server.on_stop([] { std::cout << "Server stopped" << std::endl; });

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&stop](const sys::error_code &ec) {
      if (!ec) stop();
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
