/******************************************************************************
**
** Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the aserver - which can be found at
** https://github.com/IvanPinezhaninov/aserver/.
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

#ifndef BASETEST_H
#define BASETEST_H

#include <thread>

#include <aserver/aserver.hpp>
#include <boost/asio.hpp>
#include <gtest/gtest.h>

namespace net = boost::asio;
namespace sys = boost::system;

class BaseTest : public testing::Test {
protected:
  BaseTest()
  {
    m_server.on_run([this] { m_cv.notify_one(); });

    m_server.on_stop([this] { m_cv.notify_all(); });
  }

  ~BaseTest()
  {
    stopServer();
    if (m_thread.joinable()) m_thread.join();
  }

  void startServer()
  {
    m_thread = std::thread{[this]() { m_server.run(); }};

    std::unique_lock lock{m_mutex};
    m_cv.wait(lock, [this]() { return m_server.is_running(); });
  }

  template<typename Receiver, typename... Args>
  aserver::stop_signal startServer(const typename Receiver::protocol_type::endpoint& endpoint, Args&&... args)
  {
    startServer();
    return bindReceiver<Receiver>(endpoint, std::forward<Args>(args)...);
  }

  template<typename Receiver, typename... Args>
  aserver::stop_signal bindReceiver(const typename Receiver::protocol_type::endpoint& endpoint, Args&&... args)
  {
    return m_server.bind_receiver<Receiver>(endpoint, std::forward<Args>(args)...);
  }

  void stopServer()
  {
    m_server.stop();
  }

  inline static constexpr auto serverPort = 54321;

private:
  net::io_context m_ioc;
  aserver::server m_server{m_ioc};
  std::mutex m_mutex;
  std::thread m_thread;
  std::condition_variable m_cv;
};

#endif // BASETEST_H
