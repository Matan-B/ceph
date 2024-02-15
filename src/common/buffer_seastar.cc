// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <seastar/core/sharded.hh>
#include <seastar/net/packet.hh>
#include <seastar/util/later.hh>
#include <seastar/core/sleep.hh>

#include "include/buffer_raw.h"
#include "buffer_seastar.h"

using temporary_buffer = seastar::temporary_buffer<char>;
using namespace std::chrono_literals;

namespace ceph::buffer {

class raw_seastar_foreign_ptr : public raw {
  seastar::foreign_ptr<temporary_buffer> ptr;
 public:
  raw_seastar_foreign_ptr(temporary_buffer&& buf)
    : raw(buf.get_write(), buf.size()), ptr(std::move(buf)) {}

  ~raw_seastar_foreign_ptr() {
    if (ptr.get_owner_shard() != seastar::this_shard_id()) {
      char buf[200];
      int rc = pthread_getname_np(pthread_self(), buf ,200);
      assert(rc == 0);
      if (std::string_view str(buf);
          str == "alien-store-tp" || str == "bstore_kv_final") {
        // we should let a seastar reactor destroy this memory, we are alien
        ceph_abort();
      }
    }
  }
};

class raw_seastar_local_ptr : public raw {
  temporary_buffer buf;
 public:
  raw_seastar_local_ptr(temporary_buffer&& buf)
    : raw(buf.get_write(), buf.size()), buf(std::move(buf)) {}
};

inline namespace v15_2_0 {

buffer::ptr create_delay_release(temporary_buffer&& buf) {
  buffer::ptr result = create(std::move(buf));
  buffer::ptr extra_reference(result);
  ceph_assert(result.raw_nref() == 2);
  std::ignore = seastar::do_until(
    [extender = std::move(extra_reference)]
    {return extender.raw_nref() == 1;},
    [] {return seastar::now();});
  return result;
}

buffer::ptr create(temporary_buffer&& buf) {
  return ceph::unique_leakable_ptr<buffer::raw>(
    new raw_seastar_foreign_ptr(std::move(buf)));
}

ceph::unique_leakable_ptr<buffer::raw> create_local(temporary_buffer&& buf) {
  return ceph::unique_leakable_ptr<buffer::raw>(
    new raw_seastar_local_ptr(std::move(buf)));
}

} // inline namespace v15_2_0

// buffer::ptr conversions

ptr::operator seastar::temporary_buffer<char>() &
{
  return {c_str(), _len, seastar::make_object_deleter(*this)};
}

ptr::operator seastar::temporary_buffer<char>() &&
{
  auto data = c_str();
  auto length = _len;
  return {data, length, seastar::make_object_deleter(std::move(*this))};
}

// buffer::list conversions

list::operator seastar::net::packet() &&
{
  seastar::net::packet p(_num);
  for (auto& ptr : _buffers) {
    // append each ptr as a temporary_buffer
    p = seastar::net::packet(std::move(p), std::move(ptr));
  }
  clear();
  return p;
}

} // namespace ceph::buffer

namespace {

using ceph::buffer::raw;
class raw_seastar_local_shared_ptr : public raw {
  temporary_buffer buf;
public:
  raw_seastar_local_shared_ptr(temporary_buffer& buf)
    : raw(buf.get_write(), buf.size()), buf(buf.share()) {}
};
}

buffer::ptr seastar_buffer_iterator::get_ptr(size_t len)
{
  buffer::ptr p{ceph::unique_leakable_ptr<buffer::raw>(
    new raw_seastar_local_shared_ptr{buf})};
  p.set_length(len);
  return p;
}

buffer::ptr const_seastar_buffer_iterator::get_ptr(size_t len)
{
  return buffer::ptr{ buffer::copy(get_pos_add(len), len) };
}
