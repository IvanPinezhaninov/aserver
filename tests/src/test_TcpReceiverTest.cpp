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

class Receiver final : public aserver::receiver<ip::tcp> {
public:
  void operator()(net::io_context& ioc, ip::tcp::socket socket, net::cancellation_slot slot,
                  net::yield_context yield) override
  {
    try {
      net::streambuf buffer;
      auto received = socket.async_read_some(buffer.prepare(32), net::bind_cancellation_slot(slot, yield));
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

class TcpReceiverTest : public BaseTest {
protected:
  sys::error_code ec;
  net::io_context ioc;
  ip::tcp::socket socket{ioc};
  ip::tcp::endpoint endpoint{ip::address_v4::loopback(), serverPort};
  std::string request = "Hello World!";
};

TEST_F(TcpReceiverTest, ConnectAndDisconnect)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.shutdown(ip::tcp::socket::shutdown_both, ec);
  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(TcpReceiverTest, SendAndReceive)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = net::write(socket, net::buffer(request), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, request.size());

  socket.shutdown(ip::tcp::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  std::string response(request.size(), ' ');

  auto received = net::read(socket, net::buffer(response), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, request.size());
  ASSERT_EQ(response, request);

  socket.shutdown(ip::tcp::socket::shutdown_receive, ec);
  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(TcpReceiverTest, SendEmptyPayload)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = net::write(socket, net::buffer("", 0), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, 0u);

  socket.shutdown(ip::tcp::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
