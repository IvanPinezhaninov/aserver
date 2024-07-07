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

#include <gmock/gmock.h>

#include "BaseTest.h"

namespace {

namespace ip = boost::asio::ip;

class UdpReceiver final : public aserver::receiver<ip::udp> {
public:
  ~UdpReceiver()
  {
    m_slot.clear();
  }

  void operator()(net::io_context& ioc, ip::udp::socket socket, aserver::stop_signal stop_signal,
                  net::yield_context yield) override
  {
    aserver::stop_signal::scoped_connection conn{
        stop_signal.connect([this] { m_signal.emit(net::cancellation_type::all); })};

    std::array<char, 1024> data;
    sys::error_code ec;
    ip::udp::endpoint remote_endpoint;

    auto received =
        socket.async_receive_from(net::buffer(data), remote_endpoint, net::bind_cancellation_slot(m_slot, yield[ec]));
    socket.async_send_to(net::buffer(data, received), remote_endpoint, net::bind_cancellation_slot(m_slot, yield[ec]));

    socket.close(ec);
  }

private:
  net::cancellation_signal m_signal;
  net::cancellation_slot m_slot = m_signal.slot();
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
  startServer<UdpReceiver>(endpoint);

  socket.open(ip::udp::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.send_to(net::buffer(request), endpoint, 0, ec);
  ASSERT_FALSE(ec);

  std::string response(request.size(), ' ');
  ip::udp::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(response, request);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

TEST_F(UdpReceiverTest, SendEmptyPacket)
{
  startServer<UdpReceiver>(endpoint);

  socket.open(ip::udp::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.send_to(net::buffer("", 0), endpoint, 0, ec);
  ASSERT_FALSE(ec);

  std::array<char, 1> response{};
  ip::udp::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

} // namespace
