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

namespace local = boost::asio::local;

class LocalDatagramReceiver final : public aserver::receiver<local::datagram_protocol> {
public:
  ~LocalDatagramReceiver()
  {
    m_slot.clear();
  }

  void operator()(net::io_context& ioc, local::datagram_protocol::socket socket, aserver::stop_signal cancel,
                  net::yield_context yield) override
  {
    aserver::stop_signal::scoped_connection conn{
        cancel.connect([this] { m_signal.emit(net::cancellation_type::all); })};

    std::array<char, 1024> data;
    sys::error_code ec;
    local::datagram_protocol::endpoint remote_endpoint;

    auto received =
        socket.async_receive_from(net::buffer(data), remote_endpoint, net::bind_cancellation_slot(m_slot, yield[ec]));
    socket.async_send_to(net::buffer(data, received), remote_endpoint, net::bind_cancellation_slot(m_slot, yield[ec]));

    socket.close(ec);
  }

private:
  net::cancellation_signal m_signal;
  net::cancellation_slot m_slot = m_signal.slot();
};

class LocalDatagramReceiverTest : public BaseTest {
protected:
  void TearDown() override
  {
    std::remove(server_socket_path.c_str());
    std::remove(client_socket_path.c_str());
  }

  sys::error_code ec;
  net::io_context ioc;
  local::datagram_protocol::socket socket{ioc};
  std::string server_socket_path = "/tmp/test_socket_" + std::to_string(::getpid());
  std::string client_socket_path = server_socket_path + "_client";
  local::datagram_protocol::endpoint endpoint{server_socket_path};
  std::string request = "Hello World!";
};

TEST_F(LocalDatagramReceiverTest, SendAndReceive)
{
  startServer<LocalDatagramReceiver>(endpoint);

  socket.open();
  ASSERT_TRUE(socket.is_open());
  socket.bind(local::datagram_protocol::endpoint(client_socket_path));
  socket.send_to(net::buffer(request), endpoint, 0, ec);
  ASSERT_FALSE(ec);

  std::string response(request.size(), ' ');
  local::datagram_protocol::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(response, request);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

TEST_F(LocalDatagramReceiverTest, SendEmptyPacket)
{
  startServer<LocalDatagramReceiver>(endpoint);

  socket.open();
  ASSERT_TRUE(socket.is_open());
  socket.bind(local::datagram_protocol::endpoint(client_socket_path));

  socket.send_to(net::buffer("", 0), endpoint, 0, ec);
  ASSERT_FALSE(ec);

  std::array<char, 1> response{};
  local::datagram_protocol::endpoint from;
  auto received = socket.receive_from(net::buffer(response), from, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
}

} // namespace
