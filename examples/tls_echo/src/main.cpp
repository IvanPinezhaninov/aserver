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
#include <boost/asio/ssl.hpp>
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
// socat STDIO OPENSSL:localhost:54321,verify=0

namespace {

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace ip = net::ip;
namespace ssl = net::ssl;
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

class TlsEcho final : public aserver::receiver<ip::tcp> {
public:
  TlsEcho(std::string greetings, ssl::context& sslCtx)
    : m_greetings{std::move(greetings)}
    , m_sslCtx{sslCtx}
  {
    LOG(info) << "Receiver created";
  }

  ~TlsEcho()
  {
    LOG(info) << "Receiver destroyed";
  }

  void operator()(net::io_context&, protocol_type::socket socket, net::cancellation_slot slot,
                  net::yield_context yield) override
  {
    ssl::stream<protocol_type::socket> stream{std::move(socket), m_sslCtx};
    std::optional<protocol_type::socket::endpoint_type> endpoint;

    try {
      stream.async_handshake(ssl::stream_base::server, net::bind_cancellation_slot(slot, yield));

      try {
        endpoint = stream.lowest_layer().remote_endpoint();
        LOG(info) << "Client connected: " << *endpoint;
      } catch (const std::exception&) {
        LOG(info) << "Client connected";
      }

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(stream, buffer, net::bind_cancellation_slot(slot, yield));

      auto running = true;
      net::cancellation_signal signal;
      slot.assign([&running, &signal](net::cancellation_type type) {
        running = false;
        signal.emit(type);
      });

      while (running) {
        try {
          net::async_read_until(stream, buffer, "\n", net::bind_cancellation_slot(slot, yield));
          net::async_write(stream, buffer, net::bind_cancellation_slot(slot, yield));
        } catch (const boost::system::system_error& e) {
          if (e.code() == net::error::eof) {
            LOG(info) << "Client disconnected" << logEndpoint(endpoint);
            break;
          } else if (e.code() == net::error::operation_aborted) {
            break;
          }
          throw e;
        }
      }

      closeSocket(socket);
    } catch (...) {
      closeSocket(socket);
      LOG(error) << "Receiver error" << logEndpoint(endpoint);
    }
  }

private:
  static std::string logEndpoint(const std::optional<protocol_type::socket::endpoint_type>& endpoint)
  {
    return endpoint ? ": " + endpoint->address().to_string() + ":" + std::to_string(endpoint->port()) : "";
  }

  static void closeSocket(protocol_type::socket& socket)
  {
    if (!socket.is_open()) return;
    sys::error_code ec;
    socket.shutdown(net::ip::tcp::socket::shutdown_send, ec);
    socket.close(ec);
  }

  std::string m_greetings;
  ssl::context& m_sslCtx;
  net::cancellation_signal m_signal;
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

    net::ip::tcp::endpoint endpoint{ip::tcp::v4(), 54321};
    auto stop = server.bind_receiver<TlsEcho>(
        endpoint, [&](auto e) { logError(endpoint, e); }, "Hello! I'm an echo server.", sslCtx);

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
