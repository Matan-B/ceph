#pragma once

#include <memory>
//#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <seastar/core/future.hh>

#include "crimson/common/errorator.h"
#include "osd/osd_types.h"
#include "osd/osd_op_util.h"
#include "crimson/osd/object_context.h"
#include "crimson/common/type_helpers.h"
#include "crimson/osd/shard_services.h"

namespace crimson::osd {
class ObjectContextLoader {

public:

  ObjectContextLoader(
    ShardServices &_shard_services,
    PGBackend& _be);

  //~ObjectContextLoader();

  using load_obc_ertr = crimson::errorator<
    crimson::ct_error::enoent,
    crimson::ct_error::object_corrupted>;
  using load_obc_iertr =
    ::crimson::interruptible::interruptible_errorator<
      ::crimson::osd::IOInterruptCondition,
      load_obc_ertr>;
  using interruptor = ::crimson::interruptible::interruptor<
    ::crimson::osd::IOInterruptCondition>;
  
  template<RWState::State State>
  load_obc_iertr::future<crimson::osd::ObjectContextRef>
  get_or_load_obc(
    crimson::osd::ObjectContextRef head_obc, bool existed);

  load_obc_iertr::future<crimson::osd::ObjectContextRef>
  load_obc(ObjectContextRef obc);

  load_obc_iertr::future<>
  reload_obc(crimson::osd::ObjectContext& obc) const;

  using with_obc_func_t =
    std::function<load_obc_iertr::future<> (ObjectContextRef)>;

  using obc_accessing_list_t = boost::intrusive::list<
    ObjectContext,
    ObjectContext::obc_accessing_option_t>;
  obc_accessing_list_t obc_set_accessing;

  template<RWState::State State>
  load_obc_iertr::future<> with_head_obc(hobject_t oid, with_obc_func_t&& func);

  template<RWState::State State>
  load_obc_iertr::future<> with_head_obc(
    ObjectContextRef obc,
    bool existed,
    with_obc_func_t&& func);

  template<RWState::State State>
  load_obc_iertr::future<> with_clone_obc(hobject_t oid, with_obc_func_t&& func);

private:
  ShardServices &shard_services;
  PGBackend& backend;
};
}
