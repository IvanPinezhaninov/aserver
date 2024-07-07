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
#include <aserver.hpp>
#include <boost/asio.hpp>

namespace net = boost::asio;
using namespace aserver;

class TcpReceiver : public receiver<net::ip::tcp> {
public:
  void operator()(net::io_context &ioc, net::ip::tcp::socket socket,
                  stop_signal stop_signal, net::yield_context yield) override
  {
    // Implement your custom socket handling logic here
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
