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

#include <chrono>
#include <ctime>
#include <iostream>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/core/ignore_unused.hpp>

// To connect to the server use:
// socat STDIO UDP4:localhost:12345
// and send any command

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

using namespace aserver;

void logError(const ip::udp::endpoint &endpoint, std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const sys::system_error &e) {
    std::cout << "An error occurred on endpoint: " << endpoint << ". Error: " << e.code().message() << std::endl;
  } catch (...) {
    std::cout << "An unknown error occurred on endpoint: " << endpoint << std::endl;
  }
}

class UdpTime final : public receiver<ip::udp> {
public:
  UdpTime() { std::cout << "Receiver created" << std::endl; }

  ~UdpTime() { std::cout << "Receiver destroyed" << std::endl; }

  void operator()(net::io_context &ioc, protocol_type::socket socket, stop_signal stop,
                  net::yield_context yield) override
  {
    stop_signal::connection_type conn;

    try {
      conn = stop.connect([&] { socket.cancel(); });
      while (true) {
        try {
          ip::udp::endpoint endpoint;
          std::array<char, 1> data;
          socket.async_receive_from(net::buffer(data), endpoint, yield);
          std::cout << "New client: " << endpoint << std::endl;
          net::spawn(
              net::make_strand(ioc),
              [this, &ioc, &socket, endpoint, stop](net::yield_context yield) {
                trackCoro();
                timeBroadcast(ioc, socket, std::move(endpoint), std::move(stop), yield);
              },
              [this, endpoint, stop](std::exception_ptr e) {
                untrackCoro();
                if (e) logError(endpoint, e);
              });
        } catch (const sys::system_error &e) {
          if (e.code() == net::error::operation_aborted) break;
          throw;
        }
      }
    } catch (...) {
      logError(socket.local_endpoint(), std::current_exception());
    }

    socket.close();
    conn.disconnect();
    waitForCorosFinished();
    std::cout << "Receiver closed" << std::endl;
  }

private:
  void timeBroadcast(net::io_context &ioc, protocol_type::socket &socket, protocol_type::endpoint endpoint,
                     stop_signal stop, net::yield_context yield)
  {
    auto stopped = false;
    stop_signal::connection_type conn;

    try {
      conn = stop.connect([&] { stopped = true; });
      net::steady_timer timer{ioc};

      while (!stopped) {
        try {
          using namespace std::chrono;
          auto now = system_clock::to_time_t(system_clock::now());
          auto now_tm = std::localtime(&now);
          if (now_tm != nullptr) {
            std::stringstream ss;
            ss << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S\n");
            socket.async_send_to(net::buffer(ss.str()), endpoint, yield);
          } else {
            std::cerr << "Failed to get local time" << std::endl;
          }
          timer.expires_after(std::chrono::seconds{1});
          timer.async_wait(yield);
        } catch (const sys::system_error &e) {
          if (e.code() == net::error::operation_aborted) break;
          throw;
        }
      }
    } catch (...) {
      logError(endpoint, std::current_exception());
    }

    conn.disconnect();
    std::cout << "Broadcast on endpoint: " << endpoint << " stopped" << std::endl;
  }

  void trackCoro() { m_activeCoros.fetch_add(1, std::memory_order_relaxed); }

  void untrackCoro()
  {
    if (m_activeCoros.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::lock_guard lock{m_corosMutex};
      m_corosCv.notify_all();
    }
  }

  void waitForCorosFinished()
  {
    std::unique_lock lock{m_corosMutex};
    m_corosCv.wait(lock, [this] { return m_activeCoros == 0; });
  }

  std::atomic_size_t m_activeCoros{0};
  std::mutex m_corosMutex;
  std::condition_variable m_corosCv;
};

int main(int, char **)
{
  try {
    server server{std::thread::hardware_concurrency()};
    server.on_run([] { std::cout << "Server started" << std::endl; });
    server.on_stop([] { std::cout << "Server stopped" << std::endl; });

    ip::udp::endpoint endpoint{ip::udp::v4(), 12345};
    auto stop = server.bind_receiver<UdpTime>(endpoint, [&](auto e) { logError(endpoint, e); });

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
