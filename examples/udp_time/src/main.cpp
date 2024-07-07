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

#include <chrono>
#include <ctime>
#include <iomanip>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/make_shared.hpp>

#define LOG BOOST_LOG_TRIVIAL

// To connect to the server use:
// socat STDIO UDP4:localhost:54321
// and send any command

namespace net = boost::asio;
namespace ip = net::ip;
namespace sys = boost::system;

void initLog()
{
  namespace logging = boost::log;
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;
  namespace attrs = boost::log::attributes;

  using sink_t = sinks::asynchronous_sink<sinks::text_ostream_backend>;

  auto sink = boost::make_shared<sink_t>();
  sink->locked_backend()->add_stream(boost::shared_ptr<std::ostream>(&std::cout, boost::null_deleter{}));
  sink->locked_backend()->auto_flush(true);

  sink->set_formatter(
      expr::stream << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f") << " ["
                   << std::left << std::setw(7) << std::setfill(' ') << logging::trivial::severity << "] "
                   << expr::smessage);

  logging::core::get()->add_sink(sink);
  logging::add_common_attributes();
}

void logError(const ip::udp::endpoint& endpoint, std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const sys::system_error& e) {
    LOG(error) << "An error occurred on endpoint: " << endpoint << ". Error: " << e.code().message() << " ("
               << e.code().value() << ")";
  } catch (const std::exception& e) {
    LOG(error) << "An error occurred on endpoint: " << endpoint << ". Error: " << e.what();
  } catch (...) {
    LOG(error) << "An unknown error occurred on endpoint: " << endpoint;
  }
}

class UdpTime final : public aserver::receiver<ip::udp> {
public:
  UdpTime()
  {
    LOG(info) << "Receiver created";
  }

  ~UdpTime()
  {
    LOG(info) << "Receiver destroyed";
  }

  void operator()(net::io_context& ioc, protocol_type::socket socket, aserver::stop_signal stop_signal,
                  net::yield_context yield) override
  {
    try {
      auto running = true;
      aserver::stop_signal::scoped_connection conn{stop_signal.connect([this, &running, ex = yield.get_executor()] {
        net::dispatch(ex, [this, &running] {
          running = false;
          m_socketSignal.emit(net::cancellation_type::all);
        });
      })};

      while (running) {
        try {
          ip::udp::endpoint endpoint;
          std::array<char, 1> data;

          socket.async_receive_from(net::buffer(data), endpoint,
                                    net::bind_cancellation_slot(m_socketSignal.slot(), yield));
          LOG(info) << "New client: " << endpoint;
          net::spawn(
              net::make_strand(ioc),
              [this, &ioc, &socket, endpoint, stop_signal](net::yield_context yield) {
                trackCoro();
                timeBroadcast(ioc, socket, std::move(endpoint), std::move(stop_signal), yield);
              },
              [this, endpoint](std::exception_ptr e) {
                untrackCoro();
                if (e) logError(endpoint, e);
              });
        } catch (const sys::system_error& e) {
          if (e.code() == net::error::operation_aborted) break;
          throw;
        }
      }
      socket.close();
    } catch (...) {
      waitForCorosFinished();
      LOG(error) << "Receiver abnormally closed";
      throw;
    }

    waitForCorosFinished();
    LOG(info) << "Receiver closed";
  }

private:
  void timeBroadcast(net::io_context& ioc, protocol_type::socket& socket, protocol_type::endpoint endpoint,
                     aserver::stop_signal stop_signal, net::yield_context yield)
  {
    try {
      auto running = true;
      aserver::stop_signal::scoped_connection conn{stop_signal.connect([this, &running, ex = yield.get_executor()] {
        net::dispatch(ex, [this, &running] {
          running = false;
          m_broadcastSignal.emit(net::cancellation_type::all);
        });
      })};

      net::steady_timer timer{ioc};

      while (running) {
        try {
          using namespace std::chrono;
          auto now = system_clock::to_time_t(system_clock::now());
          auto tm = std::localtime(&now);
          if (tm != nullptr) {
            std::stringstream ss;
            ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S\n");
            socket.async_send_to(net::buffer(ss.str()), endpoint, yield);
          } else {
            LOG(error) << "Failed to get local time";
          }
          timer.expires_after(std::chrono::seconds{1});
          timer.async_wait(net::bind_cancellation_slot(m_broadcastSignal.slot(), yield));
        } catch (const sys::system_error& e) {
          if (e.code() == net::error::operation_aborted) break;
          throw;
        }
      }
    } catch (...) {
      LOG(error) << "Broadcast on endpoint: " << endpoint << " abnormally stopped";
      throw;
    }

    LOG(info) << "Broadcast on endpoint: " << endpoint << " stopped";
  }

  void trackCoro()
  {
    m_activeCoros.fetch_add(1, std::memory_order_relaxed);
  }

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

  net::cancellation_signal m_socketSignal;
  net::cancellation_signal m_broadcastSignal;
  std::atomic_size_t m_activeCoros{0};
  std::mutex m_corosMutex;
  std::condition_variable m_corosCv;
};

int main()
{
  try {
    initLog();

    aserver::server server{std::thread::hardware_concurrency()};
    server.on_run([] { LOG(info) << "Server started"; });
    server.on_stop([] { LOG(info) << "Server stopped"; });

    ip::udp::endpoint endpoint{ip::udp::v4(), 54321};
    auto stop = server.bind_receiver<UdpTime>(endpoint, [&](auto e) { logError(endpoint, e); });

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&stop](const sys::error_code& ec) {
      if (!ec) stop();
    });

    net::signal_set signals{server.ioc(), SIGINT, SIGTERM};
#ifdef _WIN32
    signals.add(SIGBREAK);
#endif
    signals.async_wait([&server](const sys::error_code& ec, int) {
      if (!ec) server.stop();
    });

    server.run();
    return EXIT_SUCCESS;
  } catch (const sys::system_error& e) {
    LOG(error) << "An server error occurred: " << e.code().message() << " (" << e.code().value() << ")";
  } catch (const std::exception& e) {
    LOG(error) << "An server error occurred: " << e.what();
  } catch (...) {
    LOG(error) << "An unknown server error occurred";
  }

  return EXIT_FAILURE;
}
