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
#include <list>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/strand.hpp>

namespace aserver {

struct multiple_exception : public std::exception {
  std::vector<std::exception_ptr> exceptions;

  explicit multiple_exception(std::vector<std::exception_ptr> e)
    : exceptions{std::move(e)}
  {}

  const char* what() const noexcept override
  {
    return "Multiple exceptions occurred";
  }
};

class stop_signal {
public:
  stop_signal()
    : m_signal{std::make_shared<boost::asio::cancellation_signal>()}
  {}

  bool operator==(const stop_signal& other) const
  {
    return m_signal == other.m_signal;
  }

  void operator()() const
  {
    m_signal->emit(boost::asio::cancellation_type::all);
  }

private:
  boost::asio::cancellation_slot slot() noexcept
  {
    return m_signal->slot();
  }

  std::shared_ptr<boost::asio::cancellation_signal> m_signal;

  friend class server;
};

template<typename Derived, typename Protocol>
class receiver {
public:
  using protocol_type = Protocol;

  receiver() = default;
  receiver(const receiver&) = default;
  receiver(receiver&&) = default;
  receiver& operator=(const receiver&) = default;
  receiver& operator=(receiver&&) = default;
  virtual ~receiver() = default;

  template<typename ErrorHandler>
  void operator()(boost::asio::io_context& ioc, typename protocol_type::socket socket, ErrorHandler&& err_handler,
                  boost::asio::cancellation_slot cancel, boost::asio::yield_context yield)
  {
    static_cast<Derived&>(*this)(ioc, std::move(socket), std::forward<ErrorHandler>(err_handler), cancel, yield);
  }
};

class cancel_pool : public std::enable_shared_from_this<cancel_pool> {
private:
  struct entry;
  using iterator_type = std::list<entry>::iterator;

public:
  class cancel_token final {
  public:
    cancel_token(std::weak_ptr<cancel_pool> storage, iterator_type it, iterator_type end)
      : m_storage{std::move(storage)}
      , m_it{it}
      , m_end{end}
    {}

    ~cancel_token()
    {
      if (m_it == m_end) return;
      if (auto storage = m_storage.lock()) storage->remove_entry(m_it);
    }

    cancel_token(cancel_token&& other) noexcept
      : m_storage{std::move(other.m_storage)}
      , m_it{other.m_it}
      , m_end{other.m_end}
    {
      other.m_it = other.m_end;
    }

    cancel_token& operator=(cancel_token&& other) noexcept
    {
      if (this != &other) {
        m_storage = std::move(other.m_storage);
        m_it = other.m_it;
        m_end = other.m_end;
        other.m_it = other.m_end;
      }
      return *this;
    }

    cancel_token(const cancel_token&) = delete;
    cancel_token& operator=(const cancel_token&) = delete;

    void emit()
    {
      if (m_it == m_end) return;
      if (auto storage = m_storage.lock()) storage->emit(m_it);
    }

    boost::asio::cancellation_slot slot() const noexcept
    {
      if (m_it != m_end) return m_it->signal.slot();
      return {};
    }

  private:
    std::weak_ptr<cancel_pool> m_storage;
    iterator_type m_it;
    iterator_type m_end;
  };

  explicit cancel_pool(boost::asio::yield_context yield)
    : m_yield{yield}
  {}

  template<typename CompletionToken>
  auto make_token(CompletionToken&& token)
  {
    namespace net = boost::asio;
    auto ex = net::get_associated_executor(token);
    return net::async_compose<CompletionToken, void(cancel_token)>(
        [self = shared_from_this(), ex, token](auto& compose) {
          net::dispatch(ex, [self = std::move(self), &compose, ex, token]() {
            compose.complete(
                cancel_token{self, self->m_entries.emplace(self->m_entries.end(), token), self->m_entries.end()});
          });
        },
        token);
  }

  template<typename CompletionToken>
  void emit_all(CompletionToken&& token)
  {
    namespace net = boost::asio;
    auto ex = net::get_associated_executor(token);
    net::async_compose<CompletionToken, void()>(
        [self = shared_from_this(), ex, token](auto& compose) {
          net::dispatch(ex, [self = std::move(self), &compose] {
            for (auto it = self->m_entries.begin(); it != self->m_entries.end(); ++it)
              self->emit(it);
            compose.complete();
          });
        },
        token);
  }

private:
  struct entry {
    enum class state : uint8_t {
      idle,
      busy,
      dying,
    };

    explicit entry(boost::asio::yield_context yield)
      : signal{}
      , yield{yield}
      , state{state::idle}
    {}

    boost::asio::cancellation_signal signal;
    boost::asio::yield_context yield;
    state state;
  };

  void emit(iterator_type it)
  {
    namespace net = boost::asio;
    std::weak_ptr<cancel_pool> weak = shared_from_this();
    net::dispatch(m_yield.get_executor(), [weak, it] {
      if (auto self = weak.lock()) {
        if (it->state == entry::state::idle) {
          it->state = entry::state::busy;
          net::dispatch(it->yield.get_executor(), [self = std::move(self), it] {
            it->signal.emit(net::cancellation_type::all);
            net::dispatch(self->m_yield.get_executor(), [self = std::move(self), it] {
              switch (it->state) {
              case entry::state::idle:
                break;
              case entry::state::busy:
                it->state = entry::state::idle;
                break;
              case entry::state::dying:
                self->m_entries.erase(it);
                break;
              }
            });
          });
        }
      }
    });
  }

  void remove_entry(iterator_type it)
  {
    std::weak_ptr<cancel_pool> weak = shared_from_this();
    boost::asio::dispatch(m_yield.get_executor(), [weak, it] {
      if (auto self = weak.lock()) {
        switch (it->state) {
        case entry::state::idle:
          self->m_entries.erase(it);
          break;
        case entry::state::busy:
          it->state = entry::state::dying;
          break;
        case entry::state::dying:
          break;
        }
      }
    });
  }

  boost::asio::yield_context m_yield;
  std::list<entry> m_entries;
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
  stop_signal bind_receiver(const typename Receiver::protocol_type::endpoint& endpoint, ErrHandler&& err_handler,
                            Args&&... args)
  {
    namespace net = boost::asio;
    namespace sys = boost::system;

    auto safe_err_handler =
        SafeErrorHandler<ErrHandler>{net::make_strand(m_ioc), std::forward<ErrHandler>(err_handler)};

    stop_signal stop_signal;
    auto strand = net::make_strand(m_ioc);
    if constexpr (has_acceptor<typename Receiver::protocol_type>::value) {
      typename Receiver::protocol_type::acceptor acceptor{strand};
      acceptor.open(endpoint.protocol());
      acceptor.set_option(typename Receiver::protocol_type::socket::reuse_address{true});
      acceptor.bind(endpoint);
      acceptor.listen(Receiver::protocol_type::socket::max_listen_connections);
      track_pending_coro();
      net::spawn(
          strand,
          [this, acceptor = std::move(acceptor), safe_err_handler, stop_signal,
           args = std::forward_as_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop_signal);
            sys::error_code ec;
            do_accept<Receiver>(std::move(acceptor), safe_err_handler, stop_signal.slot(), yield[ec], std::move(args));
            if (ec) safe_err_handler(std::make_exception_ptr(sys::system_error{ec}));
            untrack_coro();
            untrack_stop_signal(stop_signal);
          },
          net::detached);
    } else {
      typename Receiver::protocol_type::socket socket{strand, endpoint};
      track_pending_coro();
      net::spawn(
          strand,
          [this, socket = std::move(socket), safe_err_handler, stop_signal,
           args = std::forward_as_tuple(std::forward<Args>(args)...)](net::yield_context yield) mutable {
            track_coro();
            track_stop_signal(stop_signal);
            sys::error_code ec;
            do_receive<Receiver>(std::move(socket), safe_err_handler, stop_signal.slot(), yield[ec], std::move(args));
            if (ec) safe_err_handler(std::make_exception_ptr(sys::system_error{ec}));
            untrack_coro();
            untrack_stop_signal(stop_signal);
          },
          net::detached);
    }

    return stop_signal;
  }

  void run()
  {
    namespace net = boost::asio;

    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
      return;

    m_work_guard.emplace(net::make_work_guard(m_ioc));

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

    if (m_on_run) net::post(m_ioc, m_on_run);

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

    if (m_pending_coros.load(std::memory_order_relaxed) != 0) {
      std::unique_lock lock{m_pending_coros_mutex};
      m_pending_coros_cv.wait(lock, [this] { return m_pending_coros.load(std::memory_order_relaxed) == 0; });
    }

    {
      std::lock_guard lock{m_stop_signals_mutex};
      for (auto& stop : m_stop_signals)
        stop();
      m_stop_signals.clear();
    }

    if (m_active_coros.load(std::memory_order_relaxed) != 0) {
      std::unique_lock lock{m_active_coros_mutex};
      m_active_coros_cv.wait(lock, [this] { return m_active_coros.load(std::memory_order_relaxed) == 0; });
    }

    m_work_guard.reset();
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

  template<typename ErrorHandler>
  class SafeErrorHandler {
  public:
    template<typename T>
    SafeErrorHandler(boost::asio::any_io_executor ex, T&& handler)
      : m_ex{ex}
      , m_handler{std::forward<T>(handler)}
    {}

    template<typename... Args>
    void operator()(Args&&... args)
    {
      boost::asio::post(m_ex, [handler = m_handler, tuple = std::make_tuple(std::forward<Args>(args)...)]() {
        try {
          std::apply(handler, std::move(tuple));
        } catch (...) {}
      });
    }

  private:
    boost::asio::any_io_executor m_ex;
    ErrorHandler m_handler;
  };

  template<typename Receiver, typename... Args, typename ErrHandler,
           typename Acceptor = typename Receiver::protocol_type::acceptor>
  void do_accept(Acceptor acceptor, ErrHandler err_handler, boost::asio::cancellation_slot slot,
                 boost::asio::yield_context yield, std::tuple<Args...> args)
  {
    namespace net = boost::asio;
    namespace sys = boost::system;

    auto running = true;
    std::atomic_bool slot_called = false;
    net::cancellation_signal signal;
    std::shared_ptr<cancel_pool> cancel_pool;

    try {
      cancel_pool = std::make_shared<aserver::cancel_pool>(yield);
      if (slot.is_connected()) {
        slot.assign([ex = yield.get_executor(), &running, &signal, &slot_called](net::cancellation_type) {
          auto exp = false;
          if (slot_called.compare_exchange_strong(exp, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            net::dispatch(ex, [&running, &signal] {
              running = false;
              signal.emit(net::cancellation_type::all);
            });
          }
        });
      }
    } catch (...) {
      running = false;
      err_handler(std::current_exception());
    }

    untrack_pending_coro();

    while (running) {
      auto strand = net::make_strand(m_ioc);
      typename Receiver::protocol_type::socket socket{strand};
      sys::error_code ec;
      acceptor.async_accept(socket, net::bind_cancellation_slot(signal.slot(), yield[ec]));
      if (ec == net::error::operation_aborted) break;
      if (ec && ec != net::error::invalid_argument) {
        err_handler(std::make_exception_ptr(sys::system_error{ec}));
        continue;
      }

      if (!socket.is_open()) continue;
      track_pending_coro();
      net::spawn(
          strand,
          [this, socket = std::move(socket), err_handler, args, cancel_pool](net::yield_context yield) mutable {
            track_coro();
            sys::error_code ec;
            auto cancel_token = cancel_pool->make_token(yield[ec]);
            if (!ec)
              do_receive<Receiver>(std::move(socket), err_handler, cancel_token.slot(), yield, std::move(args));
            else
              err_handler(std::make_exception_ptr(sys::system_error{ec}));
            untrack_coro();
          },
          net::detached);
    }

    if (slot.is_connected()) slot.clear();
    sys::error_code ec;
    cancel_pool->emit_all(yield[ec]);
    acceptor.close(ec);
  }

  template<typename Receiver, typename ErrHandler, typename... Args,
           typename Socket = typename Receiver::protocol_type::socket>
  void do_receive(Socket socket, ErrHandler err_handler, boost::asio::cancellation_slot slot,
                  boost::asio::yield_context yield, std::tuple<Args...> args)
  {
    namespace sys = boost::system;

    try {
      untrack_pending_coro();
      sys::error_code ec;
      std::make_from_tuple<Receiver>(std::move(args))(m_ioc, std::move(socket), err_handler, slot, yield[ec]);
      if (ec) err_handler(std::make_exception_ptr(sys::system_error{ec}));
    } catch (...) {
      err_handler(std::current_exception());
    }
  }

  void track_pending_coro() noexcept
  {
    m_pending_coros.fetch_add(1, std::memory_order_relaxed);
  }

  void untrack_pending_coro() noexcept
  {
    if (m_pending_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::lock_guard lock{m_pending_coros_mutex};
      m_pending_coros_cv.notify_all();
    }
  }

  void track_coro() noexcept
  {
    m_active_coros.fetch_add(1, std::memory_order_relaxed);
  }

  void untrack_coro() noexcept
  {
    if (m_active_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::lock_guard lock{m_active_coros_mutex};
      m_active_coros_cv.notify_all();
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
  std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_work_guard;
  std::vector<stop_signal> m_stop_signals;
  std::mutex m_stop_signals_mutex;
  std::atomic_size_t m_pending_coros{0};
  std::mutex m_pending_coros_mutex;
  std::condition_variable m_pending_coros_cv;
  std::atomic_size_t m_active_coros{0};
  std::mutex m_active_coros_mutex;
  std::condition_variable m_active_coros_cv;
  std::vector<std::exception_ptr> m_exceptions;
  std::mutex m_exceptions_mutex;
  std::function<void()> m_on_run;
  std::function<void()> m_on_stop;
};

} // namespace aserver

#endif // ASERVER_H
