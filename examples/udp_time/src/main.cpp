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
#include <boost/core/ignore_unused.hpp>

// To connect to the server use:
// socat STDIO UDP4:localhost:12345
// and send any command

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

using namespace aserver;

class UdpTime final : public server_worker<ip::udp> {
public:
  UdpTime() { std::cout << "Worker created" << std::endl; }

  ~UdpTime() { std::cout << "Worker destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop, net::yield_context yield) final
  {
    boost::ignore_unused(ioc);

    try {
      sys::error_code ec;
      auto stopped = false;
      stop.connect([&] {
        net::dispatch(socket.get_executor(), [&] {
          stopped = true;
          socket.close(ec);
        });
      });

      while (!stopped) {
        ip::udp::endpoint endpoint;
        std::array<char, 1> data;
        socket.async_receive_from(net::buffer(data), endpoint, yield[ec]);
        if (ec == net::error::operation_aborted && stopped) {
          std::cout << "Worker stopped" << std::endl;
          break;
        }
        if (ec)
          throw sys::system_error{ec};
        std::cout << "New client: " << endpoint << std::endl;
        net::spawn(socket.get_executor(),
                   [this, &socket, endpoint = std::move(endpoint), &stop](net::yield_context yield) {
                     timeBroadcast(socket, std::move(endpoint), stop, yield);
                   });
      }
    } catch (const std::exception &e) {
      std::cerr << "Receive error: " << e.what() << std::endl;
    }
  }

private:
  void timeBroadcast(protocol_type::socket &socket, protocol_type::endpoint endpoint, stop_signal stop,
                     net::yield_context yield)
  {
    try {
      auto stopped = false;
      stop.connect([&] { net::dispatch(socket.get_executor(), [&] { stopped = true; }); });

      while (!stopped) {
        auto now = std::time(nullptr);
        auto timeStr = std::asctime(std::localtime(&now));
        socket.async_send_to(net::buffer(std::string_view{timeStr}), endpoint, yield);
        net::steady_timer timer{socket.get_executor()};
        timer.expires_after(std::chrono::seconds{1});
        timer.async_wait(yield);
      }
    } catch (const std::exception &e) {
      std::cerr << "Broadcast error: " << e.what() << std::endl;
    }
  }
};

int main(int, char **)
{
  try {
    server server{std::thread::hardware_concurrency()};
    auto stop = server.bind_endpoint<UdpTime>({ip::udp::v4(), 12345});

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
