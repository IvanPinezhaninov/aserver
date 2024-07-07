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

namespace local = boost::asio::local;

class LocalStreamReceiver final : public aserver::receiver<local::stream_protocol> {
public:
  ~LocalStreamReceiver()
  {
    m_slot.clear();
  }

  void operator()(net::io_context& ioc, local::stream_protocol::socket socket, aserver::stop_signal stop_signal,
                  net::yield_context yield) override
  {
    aserver::stop_signal::scoped_connection conn{
        stop_signal.connect([this] { m_signal.emit(net::cancellation_type::all); })};

    net::streambuf buffer;
    sys::error_code ec;

    net::async_read(socket, buffer, net::bind_cancellation_slot(m_slot, yield[ec]));

    if (buffer.size() > 0) net::async_write(socket, buffer.data(), net::bind_cancellation_slot(m_slot, yield[ec]));

    socket.shutdown(local::stream_protocol::socket::shutdown_both, ec);
    socket.close(ec);
  }

private:
  net::cancellation_signal m_signal;
  net::cancellation_slot m_slot = m_signal.slot();
};

class LocalStreamReceiverTest : public BaseTest {
protected:
  void TearDown() override
  {
    std::remove(socket_path.c_str());
  }

  sys::error_code ec;
  net::io_context ioc;
  local::stream_protocol::socket socket{ioc};
  std::string socket_path = "/tmp/test_socket_" + std::to_string(::getpid());
  local::stream_protocol::endpoint endpoint{socket_path};
  std::string request = "Hello World!";
};

TEST_F(LocalStreamReceiverTest, ConnectAndDisconnect)
{
  startServer<LocalStreamReceiver>(endpoint);

  socket.connect(endpoint, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.shutdown(local::stream_protocol::socket::shutdown_both, ec);
  socket.close(ec);
  ASSERT_FALSE(ec);
}

TEST_F(LocalStreamReceiverTest, SendAndReceive)
{
  startServer<LocalStreamReceiver>(endpoint);

  socket.connect(endpoint);

  auto sent = net::write(socket, net::buffer(request), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, request.size());

  socket.shutdown(local::stream_protocol::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  std::string response(request.size(), ' ');
  auto received = net::read(socket, net::buffer(response), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, request.size());
  ASSERT_EQ(response, request);

  socket.shutdown(local::stream_protocol::socket::shutdown_receive, ec);
  ASSERT_FALSE(ec);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

TEST_F(LocalStreamReceiverTest, ServerClosesAfterRead)
{
  startServer<LocalStreamReceiver>(endpoint);

  socket.connect(endpoint);
  net::write(socket, net::buffer(request), ec);
  socket.shutdown(local::stream_protocol::socket::shutdown_send, ec);

  std::string response(request.size(), ' ');
  net::read(socket, net::buffer(response), ec);
  char dummy = 0;
  auto n = socket.read_some(net::buffer(&dummy, 1), ec);

  ASSERT_TRUE(ec == net::error::eof && n == 0);
}

TEST_F(LocalStreamReceiverTest, SendEmptyPayload)
{
  startServer<LocalStreamReceiver>(endpoint);

  socket.connect(endpoint);

  net::write(socket, net::buffer("", 0), ec);
  ASSERT_FALSE(ec);

  socket.shutdown(local::stream_protocol::socket::shutdown_send, ec);
  ASSERT_FALSE(ec);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

} // namespace
