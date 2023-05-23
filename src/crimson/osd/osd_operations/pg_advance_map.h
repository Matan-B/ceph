// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>
#include <seastar/core/future.hh>

#include "crimson/net/Connection.h"
#include "crimson/osd/osd_operation.h"
#include "crimson/osd/osd_operations/peering_event.h"
#include "osd/osd_types.h"
#include "crimson/common/type_helpers.h"
#include "crimson/osd/osd_connection_priv.h"
#include "crimson/osd/pg.h"

namespace ceph {
  class Formatter;
}

namespace crimson::osd {

class ShardServices;
class PG;

class PGAdvanceMap : public PhasedOperationT<PGAdvanceMap> {
public:
  static constexpr OperationTypeCode type = OperationTypeCode::pg_advance_map;
  static constexpr bool can_create() { return false; }
  spg_t get_pgid() const {
    return pg->get_pgid();
  }
  ConnectionPipeline &get_connection_pipeline() {
    return get_osd_priv(conn.get()).replicated_request_conn_pipeline;
  }

  seastar::future<> with_pg(ShardServices &shard_services, Ref<PG> pg)
  {
    //logger().debug("{}: fixme::with_pg", *this);
    return start();
    //IRef ref = this;
    //return interruptor::with_interruption([this, pg] {
    //  return start();
    //  return pg->do_update_log_missing(req, conn);
    //}, [ref](std::exception_ptr) { return seastar::now(); }, pg);
  }

  std::tuple<
    StartEvent,
    ConnectionPipeline::AwaitActive::BlockingEvent,
    ConnectionPipeline::AwaitMap::BlockingEvent,
    ConnectionPipeline::GetPG::BlockingEvent,
    PGMap::PGCreationBlockingEvent,
    OSD_OSDMapGate::OSDMapBlocker::BlockingEvent,
    PGPeeringPipeline::Process::BlockingEvent
  > tracking_events;


protected:
  ClientRequest::PGPipeline &pp(PG &pg) {
    return pg.request_pg_pipeline;
  }
  crimson::net::ConnectionRef conn;

  ShardServices &shard_services;
  Ref<PG> pg;
  PipelineHandle handle;

  std::optional<epoch_t> from;
  epoch_t min;
  epoch_t to;

  PeeringCtx rctx;
  const bool do_init;

public:
  PGAdvanceMap(crimson::net::ConnectionRef conn,
    ShardServices &shard_services, Ref<PG> pg, epoch_t from, epoch_t to,
    PeeringCtx &&rctx, bool do_init);
  ~PGAdvanceMap();

  void print(std::ostream &) const final;
  void dump_detail(ceph::Formatter *f) const final;
  seastar::future<> start();
  PipelineHandle &get_handle() { return handle; }
  epoch_t get_epoch() const { return min; } //we dont care, soley for oprtation

  seastar::future<crimson::net::ConnectionFRef> prepare_remote_submission() {
    assert(conn);
    return conn.get_foreign(
    ).then([this](auto f_conn) {
      conn.reset();
      return f_conn;
    });
  }

  void finish_remote_submission(crimson::net::ConnectionFRef _conn) {
    assert(!conn);
    conn = make_local_shared_foreign(std::move(_conn));
  }
  //std::tuple<
  //  PGPeeringPipeline::Process::BlockingEvent
  //> tracking_events;
};

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<crimson::osd::PGAdvanceMap> : fmt::ostream_formatter {};
#endif
