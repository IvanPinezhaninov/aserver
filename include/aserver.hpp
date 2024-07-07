/******************************************************************************
**
** Copyright (C) 2024 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
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

#ifndef ASERVER_H
#define ASERVER_H

#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/signals2.hpp>

namespace aserver {

template<typename T, typename = void>
struct has_acceptor : std::false_type {};

template<typename T>
struct has_acceptor<T, std::void_t<typename T::acceptor>> : std::true_type {};

class stop_signal {
public:
  using signal_type = boost::signals2::signal<void()>;

  stop_signal()
    : m_signal{std::make_shared<signal_type>()}
  {}

  boost::signals2::connection connect(const signal_type::slot_type &slot) { return m_signal->connect(slot); }

  void operator()() { (*m_signal)(); }

private:
  std::shared_ptr<signal_type> m_signal;
};

template<typename T>
class receiver {
public:
  using protocol_type = T;

  receiver() = default;
  receiver(const receiver &) = default;
  receiver(receiver &&) = default;
  receiver &operator=(const receiver &) = default;
  receiver &operator=(receiver &&) = default;
  virtual ~receiver() = default;

  virtual void operator()(boost::asio::io_context &ioc, typename protocol_type::socket socket, stop_signal stop,
                          boost::asio::yield_context yield) = 0;
};

class server {
public:
  explicit server(unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_impl{m_threads_count}
    , m_ioc{m_impl.value()}
  {}

  explicit server(boost::asio::io_context &ioc, unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_ioc{ioc}
  {}

  virtual ~server() = default;

  constexpr boost::asio::io_context &ioc() const { return m_ioc; }

  template<typename Receiver, typename... Args>
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint &endpoint, Args &&...args)
  {
    namespace net = boost::asio;
    stop_signal stop;
    if constexpr (has_acceptor<typename Receiver::protocol_type>::value) {
      typename Receiver::protocol_type::acceptor acceptor{net::make_strand(m_ioc), endpoint};
      net::spawn(m_ioc, [this, acceptor = std::move(acceptor), stop,
                         args = std::make_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
        do_accept<Receiver>(std::move(acceptor), std::move(stop), yield, std::move(args));
      });
    } else {
      typename Receiver::protocol_type::socket socket{net::make_strand(m_ioc), endpoint};
      net::spawn(m_ioc, [this, socket = std::move(socket), stop,
                         args = std::make_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
        do_receive<Receiver>(std::move(socket), std::move(stop), yield, std::move(args));
      });
    }
    return stop;
  }

  void run()
  {
    std::vector<std::thread> threads;
    threads.reserve(m_threads_count - 1);

    std::exception_ptr exception;
    std::mutex mutex;

    for (unsigned int i = 0; i < m_threads_count - 1; ++i) {
      threads.emplace_back([this, &exception, &mutex] {
        try {
          m_ioc.run();
        } catch (...) {
          std::lock_guard<std::mutex> lock{mutex};
          if (!exception) exception = std::current_exception();
        }
      });
    }

    if (m_on_run) boost::asio::post(m_ioc, m_on_run.value());

    try {
      m_ioc.run();
    } catch (...) {
      std::lock_guard<std::mutex> lock{mutex};
      if (!exception) exception = std::current_exception();
    }

    for (auto &thread : threads) {
      if (thread.joinable()) thread.join();
    }

    if (m_on_stop) (*m_on_stop)();

    if (exception) std::rethrow_exception(exception);
  }

  void stop() { m_ioc.stop(); }

  template<typename Func>
  void on_run(Func &&func)
  {
    m_on_run = std::forward<Func>(func);
  }

  template<typename Func>
  void on_stop(Func &&func)
  {
    m_on_stop = std::forward<Func>(func);
  }

private:
  template<typename Receiver, typename... Args, typename Acceptor = typename Receiver::protocol_type::acceptor>
  void do_accept(Acceptor acceptor, stop_signal stop, boost::asio::yield_context yield, std::tuple<Args...> args)
  {
    namespace net = boost::asio;
    boost::system::error_code ec;
    auto stopped = false;
    stop.connect([&] {
      net::dispatch(acceptor.get_executor(), [&] {
        stopped = true;
        acceptor.close(ec);
      });
    });

    while (!stopped) {
      typename Receiver::protocol_type::socket socket{net::make_strand(m_ioc)};
      acceptor.async_accept(socket, yield[ec]);

      if (ec == net::error::operation_aborted && stopped) break;

      if (ec) {
        std::cerr << "Error accepting connection: " << ec.message() << std::endl;
        continue;
      }

      net::spawn(m_ioc, [this, socket = std::move(socket), stop, args](net::yield_context yield) mutable {
        do_receive<Receiver>(std::move(socket), std::move(stop), yield, std::move(args));
      });
    }
  }

  template<typename Receiver, typename... Args, typename Socket = typename Receiver::protocol_type::socket>
  void do_receive(Socket socket, stop_signal stop, boost::asio::yield_context yield, std::tuple<Args...> args)
  {
    std::make_from_tuple<Receiver>(std::move(args))(m_ioc, std::move(socket), std::move(stop), yield);
  }

  unsigned int m_threads_count;
  std::optional<boost::asio::io_context> m_impl;
  boost::asio::io_context &m_ioc;
  std::optional<std::function<void()>> m_on_run;
  std::optional<std::function<void()>> m_on_stop;
};

} // namespace aserver

#endif // ASERVER_H
