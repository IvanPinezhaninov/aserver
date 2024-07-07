# aserver

[![Language C++11](https://img.shields.io/badge/language-C%2B%2B17-004482?style=flat-square)](https://en.cppreference.com/w/cpp/17)
[![MIT License](https://img.shields.io/github/license/IvanPinezhaninov/async_promise?label=license&style=flat-square)](http://opensource.org/licenses/MIT)
[![Build status](https://img.shields.io/github/actions/workflow/status/IvanPinezhaninov/aserver/build_and_test.yml?style=flat-square
)](https://github.com/IvanPinezhaninov/aserver/actions/workflows/build_and_test.yml)

## Overview

Small server library using Boost.Asio that can work with any protocol, such as TCP, UDP, Unix Domain Sockets, etc. It abstracts away the boilerplate code, allowing you to focus on implementing your custom receiver logic.

### Usage

1. **Define Your Receiver Class**: Implement a class inheriting from `receiver` for your specific protocol. Override the `operator()` to handle the socket operations.

```cpp
#include <aserver/aserver.hpp>
#include <boost/asio.hpp>

namespace ip = boost::asio::ip;
namespace net = boost::asio;
namespace sys = boost::system;

class TcpReceiver : public aserver::receiver<net::ip::tcp> {
public:
  void operator()(net::io_context &ioc, net::ip::tcp::socket socket, net::cancellation_slot slot,
                  net::yield_context yield) override
  {
    // Implement your custom socket handling logic here

    try {
      net::streambuf buffer;
      auto received = socket.async_read_some(buffer.prepare(1024), net::bind_cancellation_slot(slot, yield));
      if (received > 0) {
        buffer.commit(received);
        net::async_write(socket, buffer.data(), net::bind_cancellation_slot(slot, yield));
        buffer.consume(received);
      }
    } catch (const boost::system::system_error& e) {
      if (e.code() != net::error::eof && e.code() != net::error::operation_aborted) throw;
    }

    sys::error_code ec;
    socket.shutdown(ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
  }
};
```

2. **Set Up the Server**: Use the `server` class to start receiving connections on a specified endpoint.

```cpp
#include <aserver.hpp>

namespace net = boost::asio;
using namespace aserver;

int main()
{
  server server;
  server.bind_receiver<TcpReceiver>({net::ip::tcp::v4(), 54321});
  server.bind_receiver<UdpReceiver>({net::ip::udp::v4(), 54322});
  server.run();
  return 0;
}
```

## License

This code is distributed under the [MIT License](LICENSE)

## Author
[Ivan Pinezhaninov](mailto:ivan.pinezhaninov@gmail.com)
