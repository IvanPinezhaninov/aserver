/******************************************************************************
**
** Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
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

// To connect to the server use:
// socat STDIO TCP4:localhost:54321

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

namespace {

void logError(std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const std::exception& ex) {
    std::cout << "Error: " << ex.what() << std::endl;
  }
}

class TcpEcho final : public aserver::receiver<TcpEcho, ip::tcp> {
public:
  explicit TcpEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {}

  template<typename ErrorHandler>
  void operator()(net::io_context&, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    sys::error_code ec;
    auto endpoint = socket.remote_endpoint(ec);
    std::cout << "Client connected" << std::endl;

    net::streambuf buffer;
    std::ostream os{&buffer};
    os << m_greetings << '\n';
    net::async_write(socket, buffer, net::bind_cancellation_slot(slot, yield[ec]));
    if (ec) {
      std::cerr << "Failed to send greetings to " << endpoint << ": " << ec.message() << std::endl;
      return;
    }

    auto running = true;
    net::cancellation_signal signal;
    slot.assign([&running, &signal](net::cancellation_type type) {
      running = false;
      signal.emit(type);
    });

    while (running) {
      net::async_read_until(socket, buffer, "\n", net::bind_cancellation_slot(slot, yield[ec]));
      if (ec) {
        if (ec == net::error::eof)
          std::cout << "Client disconnected: " << endpoint << std::endl;
        else if (ec != net::error::operation_aborted)
          std::cerr << "Failed to read from " << endpoint << ": " << ec.message() << std::endl;
        break;
      }

      net::async_write(socket, buffer, net::bind_cancellation_slot(slot, yield[ec]));
      if (ec) {
        std::cerr << "Failed to send echo to " << endpoint << ": " << ec.message() << std::endl;
        break;
      }

      buffer.consume(buffer.size());
    }

    if (!socket.is_open()) return;
    socket.close(ec);
    if (ec) std::cerr << "Close failed for " << endpoint << ": " << ec.message() << std::endl;
  }

private:
  std::string m_greetings;
  net::cancellation_signal m_signal;
};

} // namespace

int main()
{
  try {
    aserver::server server{std::thread::hardware_concurrency()};
    ip::tcp::endpoint endpoint{ip::tcp::v4(), 54321};
    server.bind_receiver<TcpEcho>(endpoint, [&](std::exception_ptr e) { logError(e); }, "Hello! I'm an echo server.");

    net::signal_set signals{server.ioc(), SIGINT, SIGTERM};
    signals.async_wait([&server](const sys::error_code& ec, int) {
      if (!ec) server.stop();
    });

    server.run();
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    logError(std::current_exception());
  }

  return EXIT_FAILURE;
}
