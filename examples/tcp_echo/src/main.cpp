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
// socat STDIO TCP4:localhost:54321

namespace {

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

void logError(const ip::tcp::endpoint& endpoint, std::exception_ptr e)
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

class TcpEcho final : public aserver::receiver<ip::tcp> {
public:
  explicit TcpEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {
    LOG(info) << "Receiver created";
  }

  ~TcpEcho()
  {
    LOG(info) << "Receiver destroyed";
  }

  void operator()(net::io_context&, protocol_type::socket socket, aserver::stop_signal stop_signal,
                  net::yield_context yield) override
  {
    std::optional<protocol_type::socket::endpoint_type> endpoint;

    try {
      try {
        endpoint = socket.remote_endpoint();
        LOG(info) << "Client connected: " << *endpoint;
      } catch (const std::exception&) {
        LOG(info) << "Client connected";
      }

      auto running = true;
      aserver::stop_signal::scoped_connection conn{stop_signal.connect([this, &running, ex = yield.get_executor()] {
        net::dispatch(ex, [this, &running] {
          running = false;
          m_signal.emit(net::cancellation_type::all);
        });
      })};

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(socket, buffer, net::bind_cancellation_slot(m_signal.slot(), yield));

      while (running) {
        try {
          net::async_read_until(socket, buffer, "\n", net::bind_cancellation_slot(m_signal.slot(), yield));
          net::async_write(socket, buffer, net::bind_cancellation_slot(m_signal.slot(), yield));
        } catch (const sys::system_error& e) {
          if (e.code() == net::error::eof) {
            LOG(info) << "Client disconnected" << logEndpoint(endpoint);
            break;
          } else if (e.code() == net::error::operation_aborted) {
            break;
          }
          throw;
        }
      }
      socket.shutdown(net::ip::tcp::socket::shutdown_both);
      socket.close();
    } catch (...) {
      LOG(error) << "Receiver abnormally closed" << logEndpoint(endpoint);
      throw;
    }

    LOG(info) << "Receiver closed" << logEndpoint(endpoint);
  }

private:
  static std::string logEndpoint(const std::optional<protocol_type::socket::endpoint_type>& endpoint)
  {
    return endpoint ? ": " + endpoint->address().to_string() + ":" + std::to_string(endpoint->port()) : "";
  }

  std::string m_greetings;
  net::cancellation_signal m_signal;
};

} // namespace

int main()
{
  try {
    initLog();

    aserver::server server{std::thread::hardware_concurrency()};
    server.on_run([] { LOG(info) << "Server started"; });
    server.on_stop([] { LOG(info) << "Server stopped"; });

    net::ip::tcp::endpoint endpoint{ip::tcp::v4(), 54321};
    auto stop =
        server.bind_receiver<TcpEcho>(endpoint, [&](auto e) { logError(endpoint, e); }, "Hello! I'm an echo server.");

    net::steady_timer timer{server.ioc()};
    timer.expires_after(std::chrono::minutes{10});
    timer.async_wait([&](const sys::error_code& ec) {
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
