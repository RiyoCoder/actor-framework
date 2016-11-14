/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2016                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/network/test_multiplexer.hpp"

#include "caf/scheduler/abstract_coordinator.hpp"

#include "caf/io/scribe.hpp"
#include "caf/io/doorman.hpp"
#include "caf/io/dgram_scribe.hpp"
#include "caf/io/dgram_doorman.hpp"

namespace caf {
namespace io {
namespace network {

test_multiplexer::test_multiplexer(actor_system* sys) : multiplexer(sys) {
  CAF_ASSERT(sys != nullptr);
}

test_multiplexer::~test_multiplexer() {
  // get rid of extra ref count
  for (auto& ptr : resumables_)
    intrusive_ptr_release(ptr.get());
}

expected<connection_handle>
test_multiplexer::new_tcp_scribe(const std::string& host, uint16_t port_hint) {
  guard_type guard{mx_};
  connection_handle result;
  auto i = scribes_.find(std::make_pair(host, port_hint));
  if (i != scribes_.end()) {
    result = i->second;
    scribes_.erase(i);
  }
  return result;
}

expected<void> test_multiplexer::assign_tcp_scribe(abstract_broker* ptr,
                                                   connection_handle hdl) {
  class impl : public scribe {
  public:
    impl(abstract_broker* self, connection_handle ch, test_multiplexer* mpx)
        : scribe(self, ch),
          mpx_(mpx) {
      // nop
    }

    void configure_read(receive_policy::config config) override {
      mpx_->read_config(hdl()) = config;
    }

    void ack_writes(bool enable) override {
      mpx_->ack_writes(hdl()) = enable;
    }

    std::vector<char>& wr_buf() override {
      return mpx_->output_buffer(hdl());
    }

    std::vector<char>& rd_buf() override {
      return mpx_->input_buffer(hdl());
    }

    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }

    void flush() override {
      // nop
    }

    std::string addr() const override {
      return "test";
    }

    uint16_t port() const override {
      return static_cast<uint16_t>(hdl().id());
    }

    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }

    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }

  private:
    test_multiplexer* mpx_;
  };
  CAF_LOG_TRACE(CAF_ARG(hdl));
  auto sptr = make_counted<impl>(ptr, hdl, this);
  impl_ptr(hdl) = sptr;
  ptr->add_scribe(sptr);
  return unit;
}

connection_handle test_multiplexer::add_tcp_scribe(abstract_broker*,
                                                   native_socket) {
  std::cerr << "test_multiplexer::add_tcp_scribe called with native socket"
            << std::endl;
  abort();
}

expected<connection_handle>
test_multiplexer::add_tcp_scribe(abstract_broker* ptr, const std::string& host,
                                 uint16_t desired_port) {
  auto hdl = new_tcp_scribe(host, desired_port);
  if (!hdl)
    return std::move(hdl.error());
  assign_tcp_scribe(ptr, *hdl);
  return hdl;
}

expected<std::pair<accept_handle, uint16_t>>
test_multiplexer::new_tcp_doorman(uint16_t desired_port, const char*, bool) {
  accept_handle result;
  auto i = doormen_.find(desired_port);
  if (i != doormen_.end()) {
    result = i->second;
    doormen_.erase(i);
  }
  return std::make_pair(result, desired_port);
}

expected<void> test_multiplexer::assign_tcp_doorman(abstract_broker* ptr,
                                                    accept_handle hdl) {
  class impl : public doorman {
  public:
    impl(abstract_broker* self, accept_handle ah, test_multiplexer* mpx)
        : doorman(self, ah),
          mpx_(mpx) {
      // nop
    }

    bool new_connection() override {
      auto& mm = mpx_->pending_connects();
      auto i = mm.find(hdl());
      bool result = true;
      if (i != mm.end()) {
        result = doorman::new_connection(mpx_, i->second);
        mm.erase(i);
      }
      return result;
    }

    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }

    void launch() override {
      // nop
    }

    std::string addr() const override {
      return "test";
    }

    uint16_t port() const override {
      return mpx_->port(hdl());
    }

    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }

    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }

  private:
    test_multiplexer* mpx_;
  };
  auto dptr = make_counted<impl>(ptr, hdl, this);
  impl_ptr(hdl) = dptr;
  ptr->add_doorman(dptr);
  return unit;
}

accept_handle test_multiplexer::add_tcp_doorman(abstract_broker*,
                                                native_socket) {
  std::cerr << "test_multiplexer::add_tcp_doorman called with native socket"
            << std::endl;
  abort();
}

expected<std::pair<accept_handle, uint16_t>>
test_multiplexer::add_tcp_doorman(abstract_broker* ptr, uint16_t prt,
                                  const char* in, bool reuse_addr) {
  auto result = new_tcp_doorman(prt, in, reuse_addr);
  if (!result)
    return std::move(result.error());
  port(result->first) = prt;
  assign_tcp_doorman(ptr, result->first);
  return result;
}

expected<dgram_scribe_handle>
test_multiplexer::new_dgram_scribe(const std::string& host,
                                    uint16_t port_hint) {
  guard_type guard{mx_};
  dgram_scribe_handle result;
  auto i = dgram_scribes_.find(std::make_pair(host, port_hint));
  if (i != dgram_scribes_.end()) {
    result = i->second;
    dgram_scribes_.erase(i);
  }
  return result;
}

expected<void> test_multiplexer::assign_dgram_scribe(abstract_broker* ptr, 
                                                     dgram_scribe_handle hdl,
                                                     const std::string&,
                                                     uint16_t) {
  return assign_dgram_scribe(ptr, hdl);
}

expected<void> test_multiplexer::assign_dgram_scribe(abstract_broker* ptr,
                                                     dgram_scribe_handle hdl) {
  class impl : public dgram_scribe {
  public:
    impl(abstract_broker* self, dgram_scribe_handle dsh, test_multiplexer* mpx)
        : dgram_scribe(self, dsh),
          mpx_(mpx) {
      // nop
    }

    void configure_datagram_size(size_t buf_size) override {
      mpx_->buffer_size(hdl()) = buf_size;
    }

    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }

    void ack_writes(bool enable) override {
      mpx_->ack_writes(hdl()) = enable;
    }

    std::vector<char>& rd_buf() override {
      return mpx_->input_buffer(hdl());
    }

    std::vector<char>& wr_buf() override {
      return mpx_->output_buffer(hdl());
    }

    std::string addr() const override {
      return "test";
    }

    uint16_t port() const override {
      return mpx_->port(hdl());
    }

    void launch() {
      // nop
    }

    void flush() override {
      // nop
    }

    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }

    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    }

  private:
    test_multiplexer* mpx_;
  };
  auto dsptr = make_counted<impl>(ptr, hdl, this);
  ptr->add_dgram_scribe(dsptr);
  return unit;
}

dgram_scribe_handle test_multiplexer::add_dgram_scribe(abstract_broker*,
                                                         native_socket) {
  std::cerr << "test_multiplexer::add_dgram_scribe called with native socket"
            << std::endl;
  abort();
}

expected<dgram_scribe_handle>
test_multiplexer::add_dgram_scribe(abstract_broker*, native_socket,
                                   const std::string&, uint16_t) {
  std::cerr << "test_multiplexer::add_dgram_scribe called with native socket"
            << std::endl;
  abort();
}

expected<dgram_scribe_handle>
test_multiplexer::add_dgram_scribe(abstract_broker* ptr,
                                   const std::string& host, uint16_t prt) {
  auto result = new_dgram_scribe(host, prt);
  if (!result)
    return std::move(result.error());
  // port(*result) = prt; // TODO: remove this?
  assign_dgram_scribe(ptr, *result);
  return result;
}

expected<std::pair<dgram_doorman_handle, uint16_t>>
test_multiplexer::new_dgram_doorman(uint16_t desired_port, const char*, bool) {
  guard_type guard{mx_};
  dgram_doorman_handle result;
  auto i = dgram_doormans_.find(desired_port);
  if (i != dgram_doormans_.end()) {
    result = i->second;
    dgram_doormans_.erase(i);
  }
  return std::make_pair(result, desired_port);
}

expected<void> test_multiplexer::assign_dgram_doorman(abstract_broker* ptr,
                                                   dgram_doorman_handle hdl) {
  class impl : public dgram_doorman {
  public:
    impl(abstract_broker* self, dgram_doorman_handle dsh, test_multiplexer* mpx)
        : dgram_doorman(self, dsh),
          mpx_(mpx) {
      // nop
    }

    bool new_endpoint() override {
      // TODO: Implement me
      /*
      auto& mm = mpx_->pending_connects();
      auto i = mm.find(hdl());
      bool result = true;
      if (i != mm.end()) {
        result = doorman::new_connection(mpx_, i->second);
        mm.erase(i);
      }
      return result;
      */
      return false;
    }

    void configure_datagram_size(size_t buf_size) override {
      mpx_->buffer_size(hdl()) = buf_size;
    }

    void stop_reading() override {
      mpx_->stopped_reading(hdl()) = true;
      detach(mpx_, false);
    }

    std::vector<char>& rd_buf() override {
      return mpx_->input_buffer(hdl());
    }

    std::string addr() const override {
      return "test";
    }

    uint16_t port() const override {
      return mpx_->port(hdl());
    }

    void launch() override {
      // nop
    }

    void add_to_loop() override {
      mpx_->passive_mode(hdl()) = false;
    }

    void remove_from_loop() override {
      mpx_->passive_mode(hdl()) = true;
    } 

  private:
    test_multiplexer* mpx_;
  };
  auto dsptr = make_counted<impl>(ptr, hdl, this);
  ptr->add_dgram_doorman(dsptr);
  return unit;
}

dgram_doorman_handle test_multiplexer::add_dgram_doorman(abstract_broker*,
                                                             native_socket) {
  std::cerr << "test_multiplexer::add_dgram_doorman called with native socket"
            << std::endl;
  abort();
}

expected<std::pair<dgram_doorman_handle, uint16_t>>
test_multiplexer::add_dgram_doorman(abstract_broker* ptr, uint16_t prt,
                                      const char* in, bool reuse_addr) {
  auto result = new_dgram_doorman(prt, in, reuse_addr);
  if (!result)
    return std::move(result.error());
  port(result->first) = prt;
  assign_dgram_doorman(ptr, result->first);
  return result;
}

auto test_multiplexer::make_supervisor() -> supervisor_ptr {
  // not needed
  return nullptr;
}

void test_multiplexer::run() {
  // nop
}

void test_multiplexer::provide_scribe(std::string host, uint16_t desired_port,
                                      connection_handle hdl) {
  guard_type guard{mx_};
  scribes_.emplace(std::make_pair(std::move(host), desired_port), hdl);
}

void test_multiplexer::provide_acceptor(uint16_t desired_port,
                                        accept_handle hdl) {
  doormen_.emplace(desired_port, hdl);
  doorman_data_[hdl].port = desired_port;
}

void test_multiplexer::provide_dgram_scribe(std::string host,
                                             uint16_t desired_port,
                                             dgram_scribe_handle hdl) {
  dgram_scribes_.emplace(std::make_pair(std::move(host), desired_port), hdl);
  dgram_scribe_data_[hdl].port = desired_port;
}

void test_multiplexer::provide_dgram_doorman(uint16_t desired_port,
                                               dgram_doorman_handle hdl) {
  dgram_doormans_.emplace(std::make_pair(desired_port, hdl));
  dgram_doorman_data_[hdl].port = desired_port;
}

/// The external input buffer should be filled by
/// the test program.
test_multiplexer::buffer_type&
test_multiplexer::virtual_network_buffer(connection_handle hdl) {
  return scribe_data_[hdl].xbuf;
}

test_multiplexer::buffer_type&
test_multiplexer::output_buffer(connection_handle hdl) {
  return scribe_data_[hdl].wr_buf;
}

test_multiplexer::buffer_type&
test_multiplexer::input_buffer(connection_handle hdl) {
  return scribe_data_[hdl].rd_buf;
}

receive_policy::config& test_multiplexer::read_config(connection_handle hdl) {
  return scribe_data_[hdl].recv_conf;
}

bool& test_multiplexer::ack_writes(connection_handle hdl) {
  return scribe_data_[hdl].ack_writes;
}

bool& test_multiplexer::stopped_reading(connection_handle hdl) {
  return scribe_data_[hdl].stopped_reading;
}

bool& test_multiplexer::passive_mode(connection_handle hdl) {
  return scribe_data_[hdl].passive_mode;
}

intrusive_ptr<scribe>& test_multiplexer::impl_ptr(connection_handle hdl) {
  return scribe_data_[hdl].ptr;
}

uint16_t& test_multiplexer::port(accept_handle hdl) {
  return doorman_data_[hdl].port;
}

test_multiplexer::buffer_type&
test_multiplexer::input_buffer(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].rd_buf;
}

test_multiplexer::buffer_type&
test_multiplexer::output_buffer(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].wr_buf;
}

bool& test_multiplexer::stopped_reading(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].stopped_reading;
}

bool& test_multiplexer::ack_writes(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].ack_writes;
}

bool& test_multiplexer::passive_mode(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].passive_mode;
}

intrusive_ptr<dgram_scribe>&
test_multiplexer::impl_ptr(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].ptr;
}

uint16_t& test_multiplexer::port(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].port;
}

test_multiplexer::buffer_type&
test_multiplexer::input_buffer(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].rd_buf;
}

bool& test_multiplexer::stopped_reading(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].stopped_reading;
}

bool& test_multiplexer::passive_mode(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].passive_mode;
}

intrusive_ptr<dgram_doorman>&
test_multiplexer::impl_ptr(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].ptr;
}

uint16_t& test_multiplexer::port(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].port;
}

size_t& test_multiplexer::buffer_size(dgram_doorman_handle hdl) {
  return dgram_doorman_data_[hdl].buffer_size;
}

size_t& test_multiplexer::buffer_size(dgram_scribe_handle hdl) {
  return dgram_scribe_data_[hdl].buffer_size;
}

bool& test_multiplexer::stopped_reading(accept_handle hdl) {
  return doorman_data_[hdl].stopped_reading;
}

bool& test_multiplexer::passive_mode(accept_handle hdl) {
  return doorman_data_[hdl].passive_mode;
}

intrusive_ptr<doorman>& test_multiplexer::impl_ptr(accept_handle hdl) {
  return doorman_data_[hdl].ptr;
}

void test_multiplexer::add_pending_connect(accept_handle src,
                                           connection_handle hdl) {
  pending_connects_.emplace(src, hdl);
}

test_multiplexer::pending_connects_map& test_multiplexer::pending_connects() {
  return pending_connects_;
}

bool test_multiplexer::has_pending_scribe(std::string x, uint16_t y) {
  guard_type guard{mx_};
  return scribes_.count(std::make_pair(std::move(x), y)) > 0;
}

bool test_multiplexer::accept_connection(accept_handle hdl) {
  if (passive_mode(hdl))
    return false;
  auto& dd = doorman_data_[hdl];
  if (!dd.ptr)
    return false;
  if (!dd.ptr->new_connection())
    passive_mode(hdl) = true;
  return true;
}

void test_multiplexer::read_data(connection_handle hdl) {
  if (passive_mode(hdl))
    return;
  flush_runnables();
  scribe_data& sd = scribe_data_[hdl];
  while (!sd.ptr)
    exec_runnable();
  switch (sd.recv_conf.first) {
    case receive_policy_flag::exactly:
      while (sd.xbuf.size() >= sd.recv_conf.second) {
        sd.rd_buf.clear();
        auto first = sd.xbuf.begin();
        auto last = first + static_cast<ptrdiff_t>(sd.recv_conf.second);
        sd.rd_buf.insert(sd.rd_buf.end(), first, last);
        sd.xbuf.erase(first, last);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          passive_mode(hdl) = true;
      }
      break;
    case receive_policy_flag::at_least:
      if (sd.xbuf.size() >= sd.recv_conf.second) {
        sd.rd_buf.clear();
        sd.rd_buf.swap(sd.xbuf);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          passive_mode(hdl) = true;
      }
      break;
    case receive_policy_flag::at_most:
      auto max_bytes = static_cast<ptrdiff_t>(sd.recv_conf.second);
      while (!sd.xbuf.empty()) {
        sd.rd_buf.clear();
        auto xbuf_size = static_cast<ptrdiff_t>(sd.xbuf.size());
        auto first = sd.xbuf.begin();
        auto last = (max_bytes < xbuf_size) ? first + max_bytes : sd.xbuf.end();
        sd.rd_buf.insert(sd.rd_buf.end(), first, last);
        sd.xbuf.erase(first, last);
        if (!sd.ptr->consume(this, sd.rd_buf.data(), sd.rd_buf.size()))
          passive_mode(hdl) = true;
      }
  }
}

void test_multiplexer::virtual_send(connection_handle hdl,
                                    const buffer_type& buf) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  auto& vb = virtual_network_buffer(hdl);
  vb.insert(vb.end(), buf.begin(), buf.end());
  read_data(hdl);
}

void test_multiplexer::exec_runnable() {
  CAF_LOG_TRACE("");
  resumable_ptr ptr;
  { // critical section
    guard_type guard{mx_};
    while (resumables_.empty())
      cv_.wait(guard);
    resumables_.front().swap(ptr);
    resumables_.pop_front();
  }
  exec(ptr);
}

bool test_multiplexer::try_exec_runnable() {
  CAF_LOG_TRACE("");
  resumable_ptr ptr;
  { // critical section
    guard_type guard{mx_};
    if (resumables_.empty())
      return false;
    resumables_.front().swap(ptr);
    resumables_.pop_front();
  }
  exec(ptr);
  return true;
}

void test_multiplexer::flush_runnables() {
  CAF_LOG_TRACE("");
  // execute runnables in bursts, pick a small size to
  // minimize time in the critical section
  constexpr size_t max_runnable_count = 8;
  std::vector<resumable_ptr> runnables;
  runnables.reserve(max_runnable_count);
  // runnables can create new runnables, so we need to double-check
  // that `runnables_` is empty after each burst
  do {
    runnables.clear();
    { // critical section
      guard_type guard{mx_};
      while (!resumables_.empty() && runnables.size() < max_runnable_count) {
        runnables.emplace_back(std::move(resumables_.front()));
        resumables_.pop_front();
      }
    }
    for (auto& ptr : runnables)
      exec(ptr);
  } while (!runnables.empty());
}

void test_multiplexer::exec_later(resumable* ptr) {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE("");
  switch (ptr->subtype()) {
    case resumable::io_actor:
    case resumable::function_object: {
      std::list<resumable_ptr> tmp;
      tmp.emplace_back(ptr);
      guard_type guard{mx_};
      resumables_.splice(resumables_.end(), std::move(tmp));
      cv_.notify_all();
      break;
    }
    default:
      system().scheduler().enqueue(ptr);
  }
}

void test_multiplexer::exec(resumable_ptr& ptr) {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE("");
  switch (ptr->resume(this, 1)) {
    case resumable::resume_later:
      exec_later(ptr.get());
      break;
    case resumable::done:
    case resumable::awaiting_message:
      intrusive_ptr_release(ptr.get());
      break;
    default:
      ; // ignored
  }
}

} // namespace network
} // namespace io
} // namespace caf
