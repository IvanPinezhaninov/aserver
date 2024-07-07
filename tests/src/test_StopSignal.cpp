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

class Receiver final : public aserver::receiver<Receiver, ip::tcp> {
public:
  explicit Receiver(std::atomic_bool& running)
    : m_running{running}
  {}

  template<typename ErrorHandler>
  void operator()(net::io_context& ioc, protocol_type::socket socket, ErrorHandler, net::cancellation_slot slot,
                  net::yield_context yield)
  {
    m_running.store(true, std::memory_order_relaxed);

    sys::error_code ec;
    boost::asio::steady_timer timer{ioc};
    timer.expires_after(std::chrono::seconds{10});
    timer.async_wait(net::bind_cancellation_slot(slot, yield[ec]));

    m_running.store(false, std::memory_order_relaxed);
    timer.cancel();
    socket.close(ec);
  }

private:
  std::atomic_bool& m_running;
};

class StopSignalTest : public BaseTest {
protected:
  sys::error_code ec;
  net::io_context ioc;
  Receiver::protocol_type::socket socket{ioc};
  Receiver::protocol_type::endpoint endpoint{ip::address_v4::loopback(), serverPort};
};

TEST_F(StopSignalTest, StartStopReceiver)
{
  startServer();

  std::atomic_bool running = false;
  auto stop_signal = bindReceiver<Receiver>(endpoint, running);
  ASSERT_FALSE(running);

  socket.connect(endpoint, ec);
  for (auto i = 0; i < 500 && !running.load(std::memory_order_relaxed); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(running);

  stop_signal();
  for (auto i = 0; i < 500 && running.load(std::memory_order_relaxed); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_FALSE(running);
}

TEST_F(StopSignalTest, SeveralStopCalling)
{
  std::atomic_bool running = false;
  auto stop_signal = startServer<Receiver>(endpoint, running);

  socket.connect(endpoint, ec);
  for (auto i = 0; i < 500 && !running.load(std::memory_order_relaxed); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(running);

  for (auto i = 0; i < 100; ++i)
    stop_signal();

  for (auto i = 0; i < 500 && running.load(std::memory_order_relaxed); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_FALSE(running);
}

} // namespace
