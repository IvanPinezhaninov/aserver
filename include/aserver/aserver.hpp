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

#ifndef ASERVER_H
#define ASERVER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/signals2.hpp>

namespace aserver {

struct multiple_exception : public std::exception {
  std::vector<std::exception_ptr> exceptions;

  explicit multiple_exception(std::vector<std::exception_ptr> exceptions)
    : exceptions{std::move(exceptions)}
  {}

  const char* what() const noexcept override
  {
    return "Multiple exceptions occurred";
  }
};

class stop_signal {
public:
  using connection = boost::signals2::connection;
  using scoped_connection = boost::signals2::scoped_connection;
  using signal = boost::signals2::signal<void()>;

  stop_signal()
    : m_signal{std::make_shared<signal>()}
  {}

  virtual bool operator==(const stop_signal& other) const
  {
    return m_signal == other.m_signal;
  }

  connection connect(const signal::slot_type& slot)
  {
    return m_signal->connect(slot);
  }

  connection connect_extended(const signal::extended_slot_type& slot)
  {
    return m_signal->connect_extended(slot);
  }

  void operator()()
  {
    (*m_signal)();
  }

private:
  std::shared_ptr<signal> m_signal;
};

template<typename T>
class receiver {
public:
  using protocol_type = T;

  receiver() = default;
  receiver(const receiver&) = default;
  receiver(receiver&&) = default;
  receiver& operator=(const receiver&) = default;
  receiver& operator=(receiver&&) = default;
  virtual ~receiver() = default;

  virtual void operator()(boost::asio::io_context& ioc, typename protocol_type::socket socket, stop_signal stop_signal,
                          boost::asio::yield_context yield) = 0;
};

class server {
public:
  explicit server(unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_impl{m_threads_count}
    , m_ioc{m_impl.value()}
  {}

  explicit server(boost::asio::io_context& ioc, unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_ioc{ioc}
  {}

  server(const server&) = delete;
  server(server&&) = delete;
  server& operator=(const server&) = delete;
  server& operator=(server&&) = delete;
  virtual ~server() = default;

  constexpr boost::asio::io_context& ioc() const noexcept
  {
    return m_ioc;
  }

  template<typename Receiver, typename... Args, typename = std::enable_if_t<std::is_constructible_v<Receiver, Args...>>>
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint& endpoint, Args&&... args)
  {
    return bind_receiver<Receiver, Args...>(endpoint, [this](auto e) { handle_err(e); }, std::forward<Args>(args)...);
  }

  template<typename Receiver, typename... Args, typename ErrHandler,
           typename = std::enable_if_t<std::is_constructible_v<Receiver, Args...>>>
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint& endpoint, ErrHandler err_handler,
                            Args&&... args)
  {
    namespace net = boost::asio;

    stop_signal stop_signal;

    if constexpr (has_acceptor<typename Receiver::protocol_type>::value) {
      typename Receiver::protocol_type::acceptor acceptor{m_ioc};
      acceptor.open(endpoint.protocol());
      acceptor.set_option(typename Receiver::protocol_type::socket::reuse_address{true});
      acceptor.bind(endpoint);
      acceptor.listen(Receiver::protocol_type::socket::max_listen_connections);
      net::spawn(
          net::make_strand(m_ioc),
          [this, acceptor = std::move(acceptor), err_handler, stop_signal,
           args = std::forward_as_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop_signal);
            do_accept<Receiver>(std::move(acceptor), std::move(err_handler), std::move(stop_signal), yield,
                                std::move(args));
          },
          [this, err_handler, stop_signal](std::exception_ptr e) {
            untrack_coro();
            untrack_stop_signal(stop_signal);
            if (e) err_handler(e);
          });
    } else {
      typename Receiver::protocol_type::socket socket{m_ioc, endpoint};
      net::spawn(
          net::make_strand(m_ioc),
          [this, socket = std::move(socket), err_handler, stop_signal,
           args = std::forward_as_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop_signal);
            do_receive<Receiver>(std::move(socket), std::move(err_handler), std::move(stop_signal), yield,
                                 std::move(args));
          },
          [this, err_handler, stop_signal](std::exception_ptr e) {
            untrack_coro();
            untrack_stop_signal(stop_signal);
            if (e) err_handler(e);
          });
    }

    return stop_signal;
  }

  void run()
  {
    if (m_running.load(std::memory_order_relaxed)) return;
    m_running.store(true, std::memory_order_relaxed);

    auto threads_count = m_threads_count - 1;

    std::vector<std::thread> threads;
    threads.reserve(threads_count);

    for (auto i = 0u; i < threads_count; ++i) {
      threads.emplace_back([this] {
        try {
          m_ioc.run();
        } catch (...) {
          handle_err(std::current_exception());
        }
      });
    }

    if (m_on_run) boost::asio::post(m_ioc, m_on_run);

    try {
      m_ioc.run();
    } catch (...) {
      handle_err(std::current_exception());
    }

    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }

    if (m_on_stop) (m_on_stop)();

    if (m_exceptions.empty()) return;

    if (m_exceptions.size() == 1)
      std::rethrow_exception(m_exceptions.front());
    else
      throw multiple_exception{std::move(m_exceptions)};
  }

  void stop()
  {
    if (!m_running.load(std::memory_order_relaxed)) return;

    {
      std::lock_guard lock{m_stop_signals_mutex};
      for (auto& stop : m_stop_signals)
        stop();
      m_stop_signals.clear();
    }

    std::unique_lock lock{m_coros_mutex};
    m_coros_cv.wait(lock, [this] { return m_active_coros == 0; });
    m_ioc.stop();

    m_running.store(false, std::memory_order_relaxed);
  }

  void on_run(std::function<void()> func)
  {
    m_on_run = std::move(func);
  }

  void on_stop(std::function<void()> func)
  {
    m_on_stop = std::move(func);
  }

  bool is_running() const
  {
    return m_running.load(std::memory_order_relaxed);
  }

private:
  template<typename T, typename = void>
  struct has_acceptor : std::false_type {};

  template<typename T>
  struct has_acceptor<T, std::void_t<typename T::acceptor>> : std::true_type {};

  template<typename Receiver, typename... Args, typename ErrHandler,
           typename Acceptor = typename Receiver::protocol_type::acceptor>
  void do_accept(Acceptor acceptor, ErrHandler err_handler, stop_signal cancel, boost::asio::yield_context yield,
                 std::tuple<Args...> args)
  {
    try {
      namespace net = boost::asio;

      auto running = true;
      net::cancellation_signal op_cancel;
      auto slot = op_cancel.slot();
      stop_signal::scoped_connection conn{cancel.connect([&running, &op_cancel, executor = yield.get_executor()] {
        net::dispatch(executor, [&running, &op_cancel] {
          running = false;
          op_cancel.emit(boost::asio::cancellation_type::all);
        });
      })};

      while (running) {
        try {
          typename Receiver::protocol_type::socket socket{m_ioc};
          acceptor.async_accept(socket, net::bind_cancellation_slot(slot, yield));
          if (!socket.is_open()) continue;
          net::spawn(
              net::make_strand(m_ioc),
              [this, socket = std::move(socket), err_handler, cancel, args](net::yield_context yield) mutable {
                track_coro();
                do_receive<Receiver>(std::move(socket), std::move(err_handler), std::move(cancel), yield,
                                     std::move(args));
              },
              [this, err_handler](std::exception_ptr e) {
                untrack_coro();
                if (e) err_handler(e);
              });
        } catch (const boost::system::system_error& e) {
          if (e.code() == net::error::invalid_argument) continue;
          if (e.code() == net::error::operation_aborted) break;
          err_handler(std::current_exception());
        } catch (...) {
          err_handler(std::current_exception());
        }
      }
    } catch (...) {
      err_handler(std::current_exception());
    }

    acceptor.close();
  }

  template<typename Receiver, typename ErrHandler, typename... Args,
           typename Socket = typename Receiver::protocol_type::socket>
  void do_receive(Socket socket, ErrHandler err_handler, stop_signal stop_signal, boost::asio::yield_context yield,
                  std::tuple<Args...> args)
  {
    try {
      std::make_from_tuple<Receiver>(std::move(args))(m_ioc, std::move(socket), std::move(stop_signal), yield);
    } catch (...) {
      err_handler(std::current_exception());
    }
  }

  void track_coro()
  {
    m_active_coros.fetch_add(1, std::memory_order_relaxed);
  }

  void untrack_coro()
  {
    if (m_active_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::lock_guard lock{m_coros_mutex};
      m_coros_cv.notify_all();
    }
  }

  void track_stop_signal(stop_signal stop)
  {
    std::lock_guard lock{m_stop_signals_mutex};
    m_stop_signals.push_back(std::move(stop));
  }

  void untrack_stop_signal(const stop_signal& stop)
  {
    std::lock_guard lock{m_stop_signals_mutex};
    m_stop_signals.erase(std::remove(m_stop_signals.begin(), m_stop_signals.end(), stop), m_stop_signals.end());
  }

  void handle_err(std::exception_ptr e)
  {
    if (!e) return;
    std::lock_guard lock{m_exceptions_mutex};
    m_exceptions.push_back(e);
  }

  unsigned int m_threads_count;
  std::optional<boost::asio::io_context> m_impl;
  boost::asio::io_context& m_ioc;
  std::atomic<bool> m_running{false};
  std::vector<stop_signal> m_stop_signals;
  std::mutex m_stop_signals_mutex;
  std::atomic_size_t m_active_coros{0};
  std::mutex m_coros_mutex;
  std::condition_variable m_coros_cv;
  std::vector<std::exception_ptr> m_exceptions;
  std::mutex m_exceptions_mutex;
  std::function<void()> m_on_run;
  std::function<void()> m_on_stop;
};

} // namespace aserver

#endif // ASERVER_H
