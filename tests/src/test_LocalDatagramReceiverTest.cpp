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

class Receiver final : public aserver::receiver<local::datagram_protocol> {
public:
  void operator()(net::io_context& ioc, local::datagram_protocol::socket socket, net::cancellation_slot slot,
                  net::yield_context yield) override
  {
    try {
      std::array<char, 32> data;
      local::datagram_protocol::endpoint endpoint;
      auto received = socket.async_receive_from(net::buffer(data), endpoint, net::bind_cancellation_slot(slot, yield));
      socket.async_send_to(net::buffer(data, received), endpoint, net::bind_cancellation_slot(slot, yield));
    } catch (const boost::system::system_error& e) {
      if (e.code() != net::error::eof && e.code() != net::error::operation_aborted) throw;
    }

    sys::error_code ec;
    socket.shutdown(local::datagram_protocol::socket::shutdown_both, ec);
    socket.close(ec);
  }
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
  startServer<Receiver>(endpoint);

  socket.open();
  ASSERT_TRUE(socket.is_open());

  socket.bind(local::datagram_protocol::endpoint(client_socket_path), ec);
  ASSERT_FALSE(ec);

  auto sent = socket.send_to(net::buffer(request), endpoint, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, request.size());

  std::string response(request.size(), ' ');
  local::datagram_protocol::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, request.size());
  ASSERT_EQ(response, request);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalDatagramReceiverTest, SendEmptyPacket)
{
  startServer<Receiver>(endpoint);

  socket.open();
  ASSERT_TRUE(socket.is_open());

  socket.bind(local::datagram_protocol::endpoint(client_socket_path));
  ASSERT_FALSE(ec);

  auto sent = socket.send_to(net::buffer("", 0), endpoint, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, 0u);

  std::array<char, 1> response{};
  local::datagram_protocol::endpoint sender;
  auto received = socket.receive_from(net::buffer(response), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
