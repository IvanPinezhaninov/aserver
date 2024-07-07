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
#include <iostream>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "config.h"

// To connect to the server use:
// socat STDIO OPENSSL:localhost:54321,verify=0

namespace {

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace ip = net::ip;
namespace ssl = net::ssl;
namespace sys = boost::system;

void generateKeyAndCert(const fs::path& certPath, const fs::path& keyPath)
{
  if (fs::exists(certPath) && fs::exists(keyPath)) return;

  std::cout << "Generating certificate and key..." << std::endl;

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

  std::cout << "Successfully generated: " << certPath << " and " << keyPath << std::endl;
}

void logError(std::exception_ptr e)
{
  try {
    if (e) std::rethrow_exception(e);
  } catch (const std::exception& ex) {
    std::cout << "Error: " << ex.what() << std::endl;
  }
}

class TlsEcho final : public aserver::receiver<TlsEcho, ip::tcp> {
public:
  TlsEcho(std::string greetings, ssl::context& sslCtx)
    : m_greetings{std::move(greetings)}
    , m_sslCtx{sslCtx}
  {}

  template<typename ErrorHandler>
  void operator()(net::io_context&, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    ssl::stream<protocol_type::socket> stream{std::move(socket), m_sslCtx};

    sys::error_code ec;
    stream.async_handshake(ssl::stream_base::server, net::bind_cancellation_slot(slot, yield[ec]));
    if (ec) {
      std::cerr << "TLS handshake failed" << std::endl;
      return;
    }

    auto endpoint = stream.lowest_layer().remote_endpoint();
    std::cout << "Client connected" << std::endl;

    net::streambuf buffer;
    std::ostream os{&buffer};
    os << m_greetings << '\n';
    net::async_write(stream, buffer, net::bind_cancellation_slot(slot, yield[ec]));
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
      net::async_read_until(stream, buffer, "\n", net::bind_cancellation_slot(slot, yield[ec]));
      if (ec) {
        if (ec == net::error::eof)
          std::cout << "Client disconnected: " << endpoint << std::endl;
        else if (ec != net::error::operation_aborted)
          std::cerr << "Failed to read from " << endpoint << ": " << ec.message() << std::endl;
        break;
      }

      net::async_write(stream, buffer, net::bind_cancellation_slot(slot, yield[ec]));
      if (ec) {
        std::cerr << "Failed to send echo to " << endpoint << ": " << ec.message() << std::endl;
        break;
      }

      buffer.consume(buffer.size());
    }

    if (!stream.lowest_layer().is_open()) return;
    stream.lowest_layer().close(ec);
    if (ec) std::cerr << "Close failed for " << endpoint << ": " << ec.message() << std::endl;
  }

private:
  std::string m_greetings;
  ssl::context& m_sslCtx;
  net::cancellation_signal m_signal;
};

} // namespace

int main()
{
  try {
    auto certPath = fs::path{sslDir} / "server.crt";
    auto keyPath = fs::path{sslDir} / "server.key";
    generateKeyAndCert(certPath, keyPath);

    ssl::context sslCtx{ssl::context::tls_server};
    sslCtx.use_certificate_chain_file(certPath.string());
    sslCtx.use_private_key_file(keyPath.string(), ssl::context::pem);

    aserver::server server{std::thread::hardware_concurrency()};
    ip::tcp::endpoint endpoint{ip::tcp::v4(), 54321};
    server.bind_receiver<TlsEcho>(endpoint, [&](auto e) { logError(e); }, "Hello! I'm an echo server.", sslCtx);

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
