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

#include <filesystem>
#include <iostream>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/core/ignore_unused.hpp>

// To connect to the server use:
// socat STDIO UNIX-CONNECT:/tmp/local_echo

namespace {

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace local = net::local;
namespace sys = boost::system;

using namespace aserver;

void logError(const local::stream_protocol::endpoint &endpoint, std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const sys::system_error &e) {
    std::cout << "An error occurred on endpoint: " << endpoint << ". Error: " << e.code().message() << std::endl;
  } catch (...) {
    std::cout << "An unknown error occurred on endpoint: " << endpoint << std::endl;
  }
}

class LocalEcho final : public receiver<local::stream_protocol> {
public:
  explicit LocalEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {
    std::cout << "Receiver created" << std::endl;
  }

  ~LocalEcho() { std::cout << "Receiver destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop,
                  net::yield_context yield) override
  {
    boost::ignore_unused(ioc);
    stop_signal::connection_type conn;
    protocol_type::socket::endpoint_type endpoint;

    try {
      conn = stop.connect([&] { socket.cancel(); });

      endpoint = socket.local_endpoint();
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

fs::path socketPath()
{
  auto path = fs::temp_directory_path() / "local_echo";
  std::error_code ec;
  fs::remove(path, ec);
  return path;
}

} // namespace

int main(int, char **)
{
  try {
    auto path = socketPath();
    std::cout << "Socket path: " << path << std::endl;

    server server{std::thread::hardware_concurrency()};
    server.on_run([] { std::cout << "Server started" << std::endl; });
    server.on_stop([] { std::cout << "Server stopped" << std::endl; });

    local::stream_protocol::endpoint endpoint{path.generic_string()};
    auto stop =
        server.bind_receiver<LocalEcho>(endpoint, [&](auto e) { logError(endpoint, e); }, "Hello! I'm an echo server.");

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&stop](const sys::error_code &ec) {
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