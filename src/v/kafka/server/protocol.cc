// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "protocol.h"

#include "cluster/topics_frontend.h"
#include "config/configuration.h"
#include "kafka/server/connection_context.h"
#include "kafka/server/logger.h"
#include "kafka/server/request_context.h"
#include "kafka/server/response.h"
#include "security/scram_algorithm.h"
#include "utils/utf8.h"
#include "vlog.h"

#include <seastar/core/byteorder.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/util/log.hh>

#include <fmt/format.h>

#include <chrono>
#include <exception>
#include <limits>

namespace kafka {

protocol::protocol(
  ss::smp_service_group smp,
  ss::sharded<cluster::metadata_cache>& meta,
  ss::sharded<cluster::topics_frontend>& tf,
  ss::sharded<quota_manager>& quota,
  ss::sharded<kafka::group_router>& router,
  ss::sharded<cluster::shard_table>& tbl,
  ss::sharded<cluster::partition_manager>& pm,
  ss::sharded<coordinator_ntp_mapper>& coordinator_mapper,
  ss::sharded<fetch_session_cache>& session_cache,
  ss::sharded<cluster::id_allocator_frontend>& id_allocator_frontend,
  ss::sharded<security::credential_store>& credentials,
  ss::sharded<security::authorizer>& authorizer,
  ss::sharded<cluster::security_frontend>& sec_fe,
  std::optional<qdc_monitor::config> qdc_config,
  ss::sharded<cluster::controller_api>& controller_api) noexcept

  : _smp_group(smp)
  , _topics_frontend(tf)
  , _metadata_cache(meta)
  , _quota_mgr(quota)
  , _group_router(router)
  , _shard_table(tbl)
  , _partition_manager(pm)
  , _coordinator_mapper(coordinator_mapper)
  , _fetch_session_cache(session_cache)
  , _id_allocator_frontend(id_allocator_frontend)
  , _is_idempotence_enabled(
      config::shard_local_cfg().enable_idempotence.value())
  , _credentials(credentials)
  , _authorizer(authorizer)
  , _security_frontend(sec_fe)
  , _controller_api(controller_api) {
    if (qdc_config) {
        _qdc_mon.emplace(*qdc_config);
    }
}

ss::future<> protocol::apply(rpc::server::resources rs) {
    /*
     * if sasl authentication is not enabled then initialize the sasl state to
     * complete. this will cause auth to be skipped during request processing.
     *
     * TODO: temporarily acl authorization is enabled/disabled based on sasl
     * being enabled/disabled. it may be useful to configure them separately,
     * but this will come when identity management is introduced.
     */
    security::sasl_server sasl(
      config::shard_local_cfg().enable_sasl()
        ? security::sasl_server::sasl_state::initial
        : security::sasl_server::sasl_state::complete);

    auto ctx = ss::make_lw_shared<connection_context>(
      *this,
      std::move(rs),
      std::move(sasl),
      config::shard_local_cfg().enable_sasl());

    return ss::do_until(
             [ctx] { return ctx->is_finished_parsing(); },
             [ctx] { return ctx->process_one_request(); })
      .finally([ctx] {});
}

} // namespace kafka
