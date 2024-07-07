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

namespace local = net::local;

class Receiver final : public aserver::receiver<Receiver, local::stream_protocol> {
public:
  template<typename ErrorHandler>
  void operator()(net::io_context& ioc, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    sys::error_code ec;
    net::streambuf buffer;

    std::size_t received = socket.async_read_some(buffer.prepare(32), net::bind_cancellation_slot(slot, yield[ec]));
    if (ec) return;

    buffer.commit(received);
    net::async_write(socket, buffer.data(), net::bind_cancellation_slot(slot, yield[ec]));
    if (ec) return;

    buffer.consume(received);

    socket.shutdown(protocol_type::socket::shutdown_both, ec);
    socket.close(ec);
  }
};

class LocalStreamReceiverTest : public BaseTest {
protected:
  void TearDown() override
  {
    std::remove(socket_path.c_str());
  }

  sys::error_code ec;
  net::io_context ioc;
  Receiver::protocol_type::socket socket{ioc};
  std::string socket_path = "/tmp/test_socket_" + std::to_string(::getpid());
  Receiver::protocol_type::endpoint endpoint{socket_path};
  std::string request = "Hello World!";
};

TEST_F(LocalStreamReceiverTest, ConnectAndDisconnect)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.shutdown(Receiver::protocol_type::socket::shutdown_both, ec);
  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalStreamReceiverTest, SendAndReceive)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = net::write(socket, net::buffer(request), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, request.size());

  socket.shutdown(Receiver::protocol_type::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  std::string response(request.size(), ' ');

  auto received = net::read(socket, net::buffer(response), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, request.size());
  ASSERT_EQ(response, request);

  socket.shutdown(Receiver::protocol_type::socket::shutdown_receive, ec);
  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalStreamReceiverTest, SendEmptyPayload)
{
  startServer<Receiver>(endpoint);

  socket.connect(endpoint);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto written = net::write(socket, net::buffer("", 0), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(written, 0u);

  socket.shutdown(Receiver::protocol_type::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
