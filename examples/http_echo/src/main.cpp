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
#include <boost/beast.hpp>
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
// curl -X POST "http://localhost:8080/" -d "Hello World" -H "Content-Type: text/plain"

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
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

class HttpEcho final : public aserver::receiver<HttpEcho, ip::tcp> {
public:
  HttpEcho()
  {
    LOG(info) << "Receiver created";
  }

  ~HttpEcho()
  {
    LOG(info) << "Receiver destroyed";
  }

  template<typename ErrorHandler>
  void operator()(net::io_context&, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    beast::tcp_stream stream{std::move(socket)};
    std::optional<protocol_type::socket::endpoint_type> endpoint;

    try {
      try {
        endpoint = beast::get_lowest_layer(stream).socket().remote_endpoint();
        LOG(info) << "Client connected: " << *endpoint;
      } catch (...) {
        LOG(info) << "Client connected";
      }

      auto running = true;
      net::cancellation_signal signal;
      slot.assign([&running, &signal](net::cancellation_type type) {
        running = false;
        signal.emit(type);
      });

      net::steady_timer delay{socket.get_executor()};
      delay.expires_after(std::chrono::milliseconds{0});

      while (running) {
        try {
          stream.expires_after(std::chrono::seconds{5});

          beast::flat_buffer buffer;
          http::request_parser<http::string_body> parser;
          http::async_read(stream, buffer, parser, net::bind_cancellation_slot(signal.slot(), yield));
          const auto& req = parser.get();
          http::async_write(stream, echoResponse(req), net::bind_cancellation_slot(signal.slot(), yield));
          if (!req.keep_alive()) break;
          // Prevent stack growth if async operations complete immediately
          delay.async_wait(net::bind_cancellation_slot(signal.slot(), yield));
        } catch (const sys::system_error& e) {
          if (isDisconnectError(e)) {
            LOG(info) << "Client disconnected" << logEndpoint(endpoint);
            break;
          }
          if (e.code() == beast::error::timeout) {
            LOG(info) << "Client disconnected (timeout)" << logEndpoint(endpoint);
            break;
          }
          throw e;
        }
      }
      closeStream(stream);
    } catch (...) {
      closeStream(stream);
      LOG(error) << "Receiver error" << logEndpoint(endpoint);
    }
  }

private:
  static std::string logEndpoint(const std::optional<protocol_type::socket::endpoint_type>& endpoint)
  {
    return endpoint ? ": " + endpoint->address().to_string() + ":" + std::to_string(endpoint->port()) : "";
  }

  static http::response<http::string_body> echoResponse(const http::request<http::string_body>& req)
  {
    http::response<http::string_body> res{http::status::ok, req.version()};
    if (auto it = req.find(http::field::content_type); it != req.end()) res.set(http::field::content_type, it->value());
    res.keep_alive(req.keep_alive());
    res.body() = std::move(req.body());
    res.prepare_payload();
    return res;
  }

  static bool isDisconnectError(const boost::system::system_error& e)
  {
    const auto& ec = e.code();
    auto match = [&](boost::system::error_code code) { return ec == code && ec.category() == code.category(); };

    namespace netError = net::error;
    using httpError = http::error;

    return match(netError::eof) || match(netError::bad_descriptor) || match(netError::broken_pipe) ||
           match(netError::not_connected) || match(netError::connection_reset) || match(netError::connection_aborted) ||
           match(netError::operation_aborted) || match(httpError::end_of_stream);
  }

  static void closeStream(beast::tcp_stream& stream) noexcept
  {
    auto& socket = beast::get_lowest_layer(stream).socket();
    if (socket.is_open()) {
      sys::error_code ec;
      socket.shutdown(ip::tcp::socket::shutdown_both, ec);
      socket.close(ec);
    }
  }
};

} // namespace

int main()
{
  try {
    initLog();

    aserver::server server{std::thread::hardware_concurrency()};
    server.on_run([] { LOG(info) << "Server started"; });
    server.on_stop([] { LOG(info) << "Server stopped"; });

    net::ip::tcp::endpoint endpoint{ip::tcp::v4(), 8080};
    auto stop = server.bind_receiver<HttpEcho>(endpoint, [&](auto e) { logError(endpoint, e); });

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
