/******************************************************************************
**
** Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the arouter which can be found at
** https://github.com/IvanPinezhaninov/arouter/.
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

#include "BaseTest.h"

namespace {

namespace ip = boost::asio::ip;

class Receiver final : public aserver::receiver<ip::udp> {
public:
  void operator()(net::io_context& ioc, ip::udp::socket socket, net::cancellation_slot slot,
                  net::yield_context yield) override
  {
    try {
      std::array<char, 32> data;
      ip::udp::endpoint endpoint;
      auto received =
          socket.async_receive_from(net::buffer(data, data.size()), endpoint, net::bind_cancellation_slot(slot, yield));
      socket.async_send_to(net::buffer(data, received), endpoint, net::bind_cancellation_slot(slot, yield));
    } catch (const boost::system::system_error& e) {
      if (e.code() != net::error::eof && e.code() != net::error::operation_aborted) throw;
    }

    sys::error_code ec;
    socket.shutdown(ip::udp::socket::shutdown_both, ec);
    socket.close(ec);
  }
};

class UdpReceiverTest : public BaseTest {
protected:
  sys::error_code ec;
  net::io_context ioc;
  ip::udp::socket socket{ioc};
  ip::udp::endpoint endpoint{ip::address_v4::loopback(), serverPort};
  std::string request = "Hello World!";
};

TEST_F(UdpReceiverTest, SendAndReceive)
{
  startServer<Receiver>(endpoint);

  socket.open(ip::udp::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = socket.send_to(net::buffer(request), endpoint, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, request.size());

  std::string response(request.size(), ' ');
  ip::udp::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, request.size());
  ASSERT_EQ(response, request);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(UdpReceiverTest, SendEmptyPacket)
{
  startServer<Receiver>(endpoint);

  socket.open(ip::udp::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = socket.send_to(net::buffer("", 0), endpoint, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, 0u);

  std::array<char, 1> response{};
  ip::udp::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
