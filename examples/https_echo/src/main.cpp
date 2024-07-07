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

#include <filesystem>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/make_shared.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "config.h"

#define LOG BOOST_LOG_TRIVIAL

// To connect to the server use:
// curl -X POST "https://localhost:8443/" -d "Hello World" -H "Content-Type: text/plain" -k

namespace {

namespace fs = std::filesystem;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ip = net::ip;
namespace ssl = net::ssl;
namespace sys = boost::system;

constexpr auto timeout = std::chrono::seconds{5};

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

void generateKeyAndCert(const fs::path& certPath, const fs::path& keyPath)
{
  if (fs::exists(certPath) && fs::exists(keyPath)) return;

  LOG(info) << "Generating certificate and key...";

  static const int bits = 2048;
  static const int daysValid = 365;

  // Create key
  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* keyCtx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
  if (!keyCtx) throw std::runtime_error("Failed to create EVP_PKEY_CTX");
  if (EVP_PKEY_keygen_init(keyCtx) <= 0) throw std::runtime_error("Failed to init keygen");
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx, bits) <= 0) throw std::runtime_error("Failed to set key bits");
  if (EVP_PKEY_keygen(keyCtx, &pkey) <= 0) throw std::runtime_error("Failed to generate key");
  EVP_PKEY_CTX_free(keyCtx);

  // Create certificate
  X509* x509 = X509_new();
  if (!x509) throw std::runtime_error("Failed to create X509 object");
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), 60L * 60L * 24L * daysValid);
  X509_set_version(x509, 2);
  if (X509_set_pubkey(x509, pkey) != 1) throw std::runtime_error("Failed to assign public key to certificate");

  // Set certificate name
  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char*)"XX", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"MyOrg", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
  X509_set_issuer_name(x509, name);

  // Sign the certificate
  if (!X509_sign(x509, pkey, EVP_sha256())) throw std::runtime_error("Failed to sign the certificate");

  {
    // Write key to files
    FILE* keyFile = fopen(keyPath.string().c_str(), "wb");
    if (!keyFile || !PEM_write_PrivateKey(keyFile, pkey, nullptr, nullptr, 0, nullptr, nullptr))
      throw std::runtime_error("Failed to write private key");
    fclose(keyFile);
  }

  {
    // Write cert to files
    FILE* certFile = fopen(certPath.string().c_str(), "wb");
    if (!certFile || !PEM_write_X509(certFile, x509)) throw std::runtime_error("Failed to write certificate");
    fclose(certFile);
  }

  // Clean up
  X509_free(x509);
  EVP_PKEY_free(pkey);

  LOG(info) << "Successfully generated: " << certPath << " and " << keyPath;
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

class HttpsEcho final : public aserver::receiver<HttpsEcho, ip::tcp> {
public:
  explicit HttpsEcho(ssl::context& sslCtx)
    : m_sslCtx{sslCtx}
  {
    LOG(info) << "Receiver created";
  }

  ~HttpsEcho()
  {
    LOG(info) << "Receiver destroyed";
  }

  template<typename ErrorHandler>
  void operator()(net::io_context&, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    beast::ssl_stream<beast::tcp_stream> stream{std::move(socket), m_sslCtx};
    std::optional<protocol_type::socket::endpoint_type> endpoint;

    try {
      try {
        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.async_handshake(ssl::stream_base::server, net::bind_cancellation_slot(slot, yield));
      } catch (const boost::system::system_error& e) {
        if (e.code() == boost::asio::ssl::error::stream_truncated ||
            e.code().message().find("wrong version number") != std::string::npos) {
          LOG(warning) << "Client tried to connect without TLS";
          return;
        }
        LOG(warning) << "!!! " << e.code().message();
        return; // пробрасываем, если ошибка нераспознана
      }

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
          beast::get_lowest_layer(stream).expires_after(timeout);

          beast::flat_buffer buffer;
          http::request_parser<http::string_body> parser;
          http::async_read(stream, buffer, parser, net::bind_cancellation_slot(slot, yield));
          const auto& req = parser.get();
          http::async_write(stream, echoResponse(req), net::bind_cancellation_slot(slot, yield));
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
      closeStream(stream, slot, yield);
    } catch (...) {
      closeStream(stream, slot, yield);
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
    namespace sslError = net::ssl::error;
    using httpError = http::error;

    return match(netError::eof) || match(netError::bad_descriptor) || match(netError::broken_pipe) ||
           match(netError::not_connected) || match(netError::connection_reset) || match(netError::connection_aborted) ||
           match(netError::operation_aborted) || match(sslError::stream_truncated) || match(httpError::end_of_stream);
  }

  static void closeStream(beast::ssl_stream<beast::tcp_stream>& stream, net::cancellation_slot cancel,
                          net::yield_context yield) noexcept
  {
    auto& socket = beast::get_lowest_layer(stream).socket();
    if (!socket.is_open()) return;
    sys::error_code ec;
    stream.async_shutdown(net::bind_cancellation_slot(cancel, yield[ec]));
    socket.shutdown(ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
  }

  ssl::context& m_sslCtx;
};

} // namespace

int main()
{
  try {
    initLog();

    auto certPath = fs::path{sslDir} / "server.crt";
    auto keyPath = fs::path{sslDir} / "server.key";
    generateKeyAndCert(certPath, keyPath);

    ssl::context sslCtx{ssl::context::tls_server};
    sslCtx.use_certificate_chain_file(certPath.string());
    sslCtx.use_private_key_file(keyPath.string(), ssl::context::pem);

    aserver::server server{std::thread::hardware_concurrency()};
    server.on_run([] { LOG(info) << "Server started"; });
    server.on_stop([] { LOG(info) << "Server stopped"; });

    net::ip::tcp::endpoint endpoint{ip::tcp::v4(), 8443};
    auto stop = server.bind_receiver<HttpsEcho>(endpoint, [&](auto e) { logError(endpoint, e); }, sslCtx);

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
