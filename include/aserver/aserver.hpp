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

#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

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

  const char *what() const noexcept override { return "Multiple exceptions occurred"; }
};

class stop_signal {
public:
  using connection_type = boost::signals2::connection;
  using signal_type = boost::signals2::signal<void()>;

  stop_signal()
    : m_signal{std::make_shared<signal_type>()}
  {}

  virtual bool operator==(const stop_signal &other) const { return m_signal == other.m_signal; }

  connection_type connect(const signal_type::slot_type &slot) { return m_signal->connect(slot); }

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

  server(const server &) = delete;
  server(server &&) = delete;
  server &operator=(const server &) = delete;
  server &operator=(server &&) = delete;
  virtual ~server() = default;

  constexpr boost::asio::io_context &ioc() const noexcept { return m_ioc; }

  template<typename Receiver, typename... Args, typename = std::enable_if_t<std::is_constructible_v<Receiver, Args...>>>
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint &endpoint, Args &&...args)
  {
    return bind_receiver<Receiver, Args...>(endpoint, [this](auto e) { handle_err(e); }, std::forward<Args>(args)...);
  }

  template<typename Receiver, typename... Args, typename ErrHandler,
           typename = std::enable_if_t<std::is_constructible_v<Receiver, Args...>>>
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint &endpoint, ErrHandler err_handler,
                            Args &&...args)
  {
    namespace net = boost::asio;

    stop_signal stop;

    if constexpr (has_acceptor<typename Receiver::protocol_type>::value) {
      typename Receiver::protocol_type::acceptor acceptor{m_ioc, endpoint};
      net::spawn(
          net::make_strand(m_ioc),
          [this, acceptor = std::move(acceptor), err_handler, stop,
           args = std::make_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop);
            do_accept<Receiver>(std::move(acceptor), std::move(err_handler), std::move(stop), yield, std::move(args));
          },
          [this, err_handler, stop](std::exception_ptr e) {
            untrack_coro();
            untrack_stop_signal(stop);
            if (e) err_handler(e);
          });
    } else {
      typename Receiver::protocol_type::socket socket{m_ioc, endpoint};
      net::spawn(
          net::make_strand(m_ioc),
          [this, socket = std::move(socket), err_handler, stop,
           args = std::make_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop);
            do_receive<Receiver>(std::move(socket), std::move(err_handler), std::move(stop), yield, std::move(args));
          },
          [this, err_handler, stop](std::exception_ptr e) {
            untrack_coro();
            untrack_stop_signal(stop);
            if (e) err_handler(e);
          });
    }

    return stop;
  }

  void run()
  {
    if (m_running) return;
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

    for (auto &thread : threads) {
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
    if (!m_running) return;

    for (auto &stop : m_stops)
      stop();

    std::unique_lock lock{m_coros_mutex};
    m_coros_cv.wait(lock, [this] { return m_active_coros == 0; });
    m_ioc.stop();

    m_running.store(false, std::memory_order_relaxed);
  }

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
  template<typename T, typename = void>
  struct has_acceptor : std::false_type {};

  template<typename T>
  struct has_acceptor<T, std::void_t<typename T::acceptor>> : std::true_type {};

  template<typename Receiver, typename... Args, typename ErrHandler,
           typename Acceptor = typename Receiver::protocol_type::acceptor>
  void do_accept(Acceptor acceptor, ErrHandler err_handler, stop_signal stop, boost::asio::yield_context yield,
                 std::tuple<Args...> args)
  {
    stop_signal::connection_type conn;

    try {
      conn = stop.connect([&] { acceptor.cancel(); });
      while (true) {
        namespace net = boost::asio;
        try {
          typename Receiver::protocol_type::socket socket{m_ioc};
          acceptor.async_accept(socket, yield);
          net::spawn(
              net::make_strand(m_ioc),
              [this, socket = std::move(socket), err_handler, stop, args](net::yield_context yield) mutable {
                track_coro();
                do_receive<Receiver>(std::move(socket), std::move(err_handler), std::move(stop), yield,
                                     std::move(args));
              },
              [this, err_handler](std::exception_ptr e) {
                untrack_coro();
                if (e) err_handler(e);
              });
        } catch (const boost::system::system_error &e) {
          if (e.code() == net::error::operation_aborted) break;
          throw;
        } catch (...) {
          throw;
        }
      }
    } catch (...) {
      err_handler(std::current_exception());
    }

    acceptor.close();
    conn.disconnect();
  }

  template<typename Receiver, typename ErrHandler, typename... Args,
           typename Socket = typename Receiver::protocol_type::socket>
  void do_receive(Socket socket, ErrHandler err_handler, stop_signal stop, boost::asio::yield_context yield,
                  std::tuple<Args...> args)
  {
    try {
      std::make_from_tuple<Receiver>(std::move(args))(m_ioc, std::move(socket), std::move(stop), yield);
    } catch (...) {
      err_handler(std::current_exception());
    }
  }

  void track_coro() { m_active_coros.fetch_add(1, std::memory_order_relaxed); }

  void untrack_coro()
  {
    if (m_active_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::lock_guard lock{m_coros_mutex};
      m_coros_cv.notify_all();
    }
  }

  void track_stop_signal(stop_signal stop)
  {
    std::lock_guard lock{m_stops_mutex};
    m_stops.push_back(std::move(stop));
  }

  void untrack_stop_signal(const stop_signal &stop)
  {
    std::lock_guard lock{m_stops_mutex};
    m_stops.erase(std::remove(m_stops.begin(), m_stops.end(), stop), m_stops.end());
  }

  void handle_err(std::exception_ptr e)
  {
    if (!e) return;
    std::lock_guard lock{m_exceptions_mutex};
    m_exceptions.push_back(e);
  }

  unsigned int m_threads_count;
  std::optional<boost::asio::io_context> m_impl;
  boost::asio::io_context &m_ioc;
  std::atomic<bool> m_running{false};
  std::vector<stop_signal> m_stops;
  std::mutex m_stops_mutex;
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
