// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/pipeline/fragment_executor.h"

#include <unordered_map>

#include "common/config.h"
#include "exec/cross_join_node.h"
#include "exec/exchange_node.h"
#include "exec/olap_scan_node.h"
#include "exec/pipeline/adaptive/event.h"
#include "exec/pipeline/chunk_accumulate_operator.h"
#include "exec/pipeline/exchange/exchange_sink_operator.h"
#include "exec/pipeline/exchange/multi_cast_local_exchange.h"
#include "exec/pipeline/exchange/sink_buffer.h"
#include "exec/pipeline/fragment_context.h"
#include "exec/pipeline/noop_sink_operator.h"
#include "exec/pipeline/olap_table_sink_operator.h"
#include "exec/pipeline/pipeline_builder.h"
#include "exec/pipeline/pipeline_driver_executor.h"
#include "exec/pipeline/result_sink_operator.h"
#include "exec/pipeline/scan/connector_scan_operator.h"
#include "exec/pipeline/scan/morsel.h"
#include "exec/pipeline/scan/scan_operator.h"
#include "exec/pipeline/sink/blackhole_table_sink_operator.h"
#include "exec/pipeline/sink/dictionary_cache_sink_operator.h"
#include "exec/pipeline/sink/export_sink_operator.h"
#include "exec/pipeline/sink/file_sink_operator.h"
#include "exec/pipeline/sink/hive_table_sink_operator.h"
#include "exec/pipeline/sink/iceberg_table_sink_operator.h"
#include "exec/pipeline/sink/memory_scratch_sink_operator.h"
#include "exec/pipeline/sink/mysql_table_sink_operator.h"
#include "exec/pipeline/sink/table_function_table_sink_operator.h"
#include "exec/pipeline/stream_pipeline_driver.h"
#include "exec/scan_node.h"
#include "exec/tablet_sink.h"
#include "exec/workgroup/work_group.h"
#include "gutil/casts.h"
#include "gutil/map_util.h"
#include "runtime/blackhole_table_sink.h"
#include "runtime/data_stream_mgr.h"
#include "runtime/data_stream_sender.h"
#include "runtime/descriptors.h"
#include "runtime/dictionary_cache_sink.h"
#include "runtime/exec_env.h"
#include "runtime/export_sink.h"
#include "runtime/hive_table_sink.h"
#include "runtime/iceberg_table_sink.h"
#include "runtime/memory_scratch_sink.h"
#include "runtime/multi_cast_data_stream_sink.h"
#include "runtime/mysql_table_sink.h"
#include "runtime/result_sink.h"
#include "runtime/stream_load/stream_load_context.h"
#include "runtime/stream_load/transaction_mgr.h"
#include "runtime/table_function_table_sink.h"
#include "util/debug/query_trace.h"
#include "util/runtime_profile.h"
#include "util/time.h"
#include "util/uid_util.h"

namespace starrocks::pipeline {

using WorkGroupManager = workgroup::WorkGroupManager;
using WorkGroup = workgroup::WorkGroup;
using WorkGroupPtr = workgroup::WorkGroupPtr;
using PipelineGroupMap = std::unordered_map<SourceOperatorFactory*, std::vector<Pipeline*>>;

/// UnifiedExecPlanFragmentParams.
const std::vector<TScanRangeParams> UnifiedExecPlanFragmentParams::_no_scan_ranges;
const PerDriverScanRangesMap UnifiedExecPlanFragmentParams::_no_scan_ranges_per_driver_seq;

const std::vector<TScanRangeParams>& UnifiedExecPlanFragmentParams::scan_ranges_of_node(TPlanNodeId node_id) const {
    return FindWithDefault(_unique_request.params.per_node_scan_ranges, node_id, _no_scan_ranges);
}

const PerDriverScanRangesMap& UnifiedExecPlanFragmentParams::per_driver_seq_scan_ranges_of_node(
        TPlanNodeId node_id) const {
    if (!_unique_request.params.__isset.node_to_per_driver_seq_scan_ranges) {
        return _no_scan_ranges_per_driver_seq;
    }

    return FindWithDefault(_unique_request.params.node_to_per_driver_seq_scan_ranges, node_id,
                           _no_scan_ranges_per_driver_seq);
}

const TDataSink& UnifiedExecPlanFragmentParams::output_sink() const {
    if (_unique_request.fragment.__isset.output_sink) {
        return _unique_request.fragment.output_sink;
    }
    return _common_request.fragment.output_sink;
}

/// FragmentExecutor.
FragmentExecutor::FragmentExecutor() {
    _fragment_start_time = MonotonicNanos();
}

Status FragmentExecutor::_prepare_query_ctx(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) {
    // prevent an identical fragment instance from multiple execution caused by FE's
    // duplicate invocations of rpc exec_plan_fragment.
    const auto& params = request.common().params;
    const auto& query_id = params.query_id;
    const auto& fragment_instance_id = request.fragment_instance_id();
    const auto& query_options = request.common().query_options;

    auto&& existing_query_ctx = exec_env->query_context_mgr()->get(query_id);
    if (existing_query_ctx) {
        auto&& existingfragment_ctx = existing_query_ctx->fragment_mgr()->get(fragment_instance_id);
        if (existingfragment_ctx) {
            return Status::DuplicateRpcInvocation("Duplicate invocations of exec_plan_fragment");
        }
    }

    _query_ctx = exec_env->query_context_mgr()->get_or_register(query_id);
    _query_ctx->set_exec_env(exec_env);
    if (params.__isset.instances_number) {
        _query_ctx->set_total_fragments(params.instances_number);
    }

    _query_ctx->set_delivery_expire_seconds(_calc_delivery_expired_seconds(request));
    _query_ctx->set_query_expire_seconds(_calc_query_expired_seconds(request));
    // initialize query's deadline
    _query_ctx->extend_delivery_lifetime();
    _query_ctx->extend_query_lifetime();

    if (query_options.__isset.enable_pipeline_level_shuffle) {
        _query_ctx->set_enable_pipeline_level_shuffle(query_options.enable_pipeline_level_shuffle);
    }
    if (query_options.__isset.enable_profile && query_options.enable_profile) {
        _query_ctx->set_enable_profile();
    }
    if (query_options.__isset.big_query_profile_threshold) {
        _query_ctx->set_big_query_profile_threshold(query_options.big_query_profile_threshold,
                                                    query_options.big_query_profile_threshold_unit);
    }
    if (query_options.__isset.pipeline_profile_level) {
        _query_ctx->set_profile_level(query_options.pipeline_profile_level);
    }
    if (query_options.__isset.runtime_profile_report_interval) {
        _query_ctx->set_runtime_profile_report_interval(std::max(1L, query_options.runtime_profile_report_interval));
    }

    bool enable_query_trace = false;
    if (query_options.__isset.enable_query_debug_trace && query_options.enable_query_debug_trace) {
        enable_query_trace = true;
    }
    _query_ctx->set_query_trace(std::make_shared<starrocks::debug::QueryTrace>(query_id, enable_query_trace));

    return Status::OK();
}

Status FragmentExecutor::_prepare_fragment_ctx(const UnifiedExecPlanFragmentParams& request) {
    const auto& coord = request.common().coord;
    const auto& query_id = request.common().params.query_id;
    const auto& fragment_instance_id = request.fragment_instance_id();
    const auto& is_stream_pipeline = request.is_stream_pipeline();

    _fragment_ctx = std::make_shared<FragmentContext>();

    _fragment_ctx->set_query_id(query_id);
    _fragment_ctx->set_fragment_instance_id(fragment_instance_id);
    _fragment_ctx->set_fe_addr(coord);
    _fragment_ctx->set_is_stream_pipeline(is_stream_pipeline);
    if (request.common().__isset.adaptive_dop_param) {
        _fragment_ctx->set_enable_adaptive_dop(true);
        const auto& tadaptive_dop_param = request.common().adaptive_dop_param;
        auto& adaptive_dop_param = _fragment_ctx->adaptive_dop_param();
        adaptive_dop_param.max_block_rows_per_driver_seq = tadaptive_dop_param.max_block_rows_per_driver_seq;
        adaptive_dop_param.max_output_amplification_factor = tadaptive_dop_param.max_output_amplification_factor;
    }

    return Status::OK();
}

Status FragmentExecutor::_prepare_workgroup(const UnifiedExecPlanFragmentParams& request) {
    WorkGroupPtr wg;
    if (!request.common().__isset.workgroup || request.common().workgroup.id == WorkGroup::DEFAULT_WG_ID) {
        wg = WorkGroupManager::instance()->get_default_workgroup();
    } else if (request.common().workgroup.id == WorkGroup::DEFAULT_MV_WG_ID) {
        wg = WorkGroupManager::instance()->get_default_mv_workgroup();
    } else {
        wg = std::make_shared<WorkGroup>(request.common().workgroup);
        wg = WorkGroupManager::instance()->add_workgroup(wg);
    }
    DCHECK(wg != nullptr);

    const auto& query_options = request.common().query_options;
    bool enable_group_level_query_queue = false;
    if (query_options.__isset.query_queue_options) {
        const auto& queue_options = query_options.query_queue_options;
        enable_group_level_query_queue =
                queue_options.__isset.enable_group_level_query_queue && queue_options.enable_group_level_query_queue;
    }
    RETURN_IF_ERROR(_query_ctx->init_query_once(wg.get(), enable_group_level_query_queue));

    _fragment_ctx->set_workgroup(wg);
    _wg = wg;

    return Status::OK();
}

Status FragmentExecutor::_prepare_runtime_state(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) {
    const auto& params = request.common().params;
    const auto& query_id = params.query_id;
    const auto& fragment_instance_id = request.fragment_instance_id();
    const auto& query_globals = request.common().query_globals;
    const auto& query_options = request.common().query_options;
    const auto& t_desc_tbl = request.common().desc_tbl;
    auto& wg = _wg;

    _fragment_ctx->set_runtime_state(
            std::make_unique<RuntimeState>(query_id, fragment_instance_id, query_options, query_globals, exec_env));
    auto* runtime_state = _fragment_ctx->runtime_state();
    runtime_state->set_enable_pipeline_engine(true);
    runtime_state->set_fragment_ctx(_fragment_ctx.get());
    runtime_state->set_query_ctx(_query_ctx);

    // Only consider the `query_mem_limit` variable
    // If query_mem_limit is <= 0, it would set to -1, which means no limit
    auto* parent_mem_tracker = wg->mem_tracker();
    int64_t option_query_mem_limit = query_options.__isset.query_mem_limit ? query_options.query_mem_limit : -1;
    if (option_query_mem_limit <= 0) option_query_mem_limit = -1;
    int64_t big_query_mem_limit = wg->use_big_query_mem_limit() ? wg->big_query_mem_limit() : -1;
    int64_t spill_mem_limit_bytes = -1;
    if (query_options.__isset.enable_spill && query_options.enable_spill && option_query_mem_limit > 0) {
        spill_mem_limit_bytes = option_query_mem_limit * query_options.spill_mem_limit_threshold;
    }
    _query_ctx->init_mem_tracker(option_query_mem_limit, parent_mem_tracker, big_query_mem_limit, spill_mem_limit_bytes,
                                 wg.get(), runtime_state);

    auto query_mem_tracker = _query_ctx->mem_tracker();
    SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(query_mem_tracker.get());

    int func_version = request.common().__isset.func_version
                               ? request.common().func_version
                               : TFunctionVersion::type::RUNTIME_FILTER_SERIALIZE_VERSION_2;
    runtime_state->set_func_version(func_version);
    runtime_state->init_mem_trackers(query_mem_tracker);
    runtime_state->set_be_number(request.backend_num());

    // RuntimeFilterWorker::open_query is idempotent
    const TRuntimeFilterParams* runtime_filter_params = nullptr;
    if (request.unique().params.__isset.runtime_filter_params &&
        !request.unique().params.runtime_filter_params.id_to_prober_params.empty()) {
        runtime_filter_params = &request.unique().params.runtime_filter_params;
    } else if (request.common().params.__isset.runtime_filter_params &&
               !request.common().params.runtime_filter_params.id_to_prober_params.empty()) {
        runtime_filter_params = &request.common().params.runtime_filter_params;
    }
    if (runtime_filter_params != nullptr) {
        _query_ctx->set_is_runtime_filter_coordinator(true);
        exec_env->runtime_filter_worker()->open_query(query_id, query_options, *runtime_filter_params, true);
    }
    _fragment_ctx->prepare_pass_through_chunk_buffer();

    auto* obj_pool = runtime_state->obj_pool();
    // Set up desc tbl
    DescriptorTbl* desc_tbl = nullptr;
    if (t_desc_tbl.__isset.is_cached) {
        if (t_desc_tbl.is_cached) {
            desc_tbl = _query_ctx->desc_tbl();
            if (desc_tbl == nullptr) {
                return Status::Cancelled("Query terminates prematurely");
            }
        } else {
            RETURN_IF_ERROR(DescriptorTbl::create(runtime_state, _query_ctx->object_pool(), t_desc_tbl, &desc_tbl,
                                                  runtime_state->chunk_size()));
            _query_ctx->set_desc_tbl(desc_tbl);
        }
    } else {
        RETURN_IF_ERROR(
                DescriptorTbl::create(runtime_state, obj_pool, t_desc_tbl, &desc_tbl, runtime_state->chunk_size()));
    }
    runtime_state->set_desc_tbl(desc_tbl);
    if (query_options.__isset.enable_spill && query_options.enable_spill) {
        RETURN_IF_ERROR(_query_ctx->init_spill_manager(query_options));
    }
    _fragment_ctx->init_jit_profile();
    return Status::OK();
}

uint32_t FragmentExecutor::_calc_dop(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) const {
    int32_t degree_of_parallelism = request.pipeline_dop();
    return exec_env->calc_pipeline_dop(degree_of_parallelism);
}

uint32_t FragmentExecutor::_calc_sink_dop(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) const {
    int32_t degree_of_parallelism = request.pipeline_sink_dop();
    return exec_env->calc_pipeline_sink_dop(degree_of_parallelism);
}

int FragmentExecutor::_calc_delivery_expired_seconds(const UnifiedExecPlanFragmentParams& request) const {
    const auto& query_options = request.common().query_options;

    int expired_seconds = QueryContext::DEFAULT_EXPIRE_SECONDS;
    if (query_options.__isset.query_delivery_timeout) {
        if (query_options.__isset.query_timeout) {
            expired_seconds = std::min(query_options.query_timeout, query_options.query_delivery_timeout);
        } else {
            expired_seconds = query_options.query_delivery_timeout;
        }
    } else if (query_options.__isset.query_timeout) {
        expired_seconds = query_options.query_timeout;
    }

    return std::max<int>(1, expired_seconds);
}

int FragmentExecutor::_calc_query_expired_seconds(const UnifiedExecPlanFragmentParams& request) const {
    const auto& query_options = request.common().query_options;

    if (query_options.__isset.query_timeout) {
        return std::max<int>(1, query_options.query_timeout);
    }

    return QueryContext::DEFAULT_EXPIRE_SECONDS;
}

Status FragmentExecutor::_prepare_exec_plan(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) {
    auto* runtime_state = _fragment_ctx->runtime_state();
    auto* obj_pool = runtime_state->obj_pool();
    const DescriptorTbl& desc_tbl = runtime_state->desc_tbl();
    const auto& params = request.common().params;
    const auto& fragment = request.common().fragment;
    const auto dop = _calc_dop(exec_env, request);
    const auto& query_options = request.common().query_options;
    const int chunk_size = runtime_state->chunk_size();

    bool enable_shared_scan = request.common().__isset.enable_shared_scan && request.common().enable_shared_scan;
    bool enable_tablet_internal_parallel =
            query_options.__isset.enable_tablet_internal_parallel && query_options.enable_tablet_internal_parallel;
    TTabletInternalParallelMode::type tablet_internal_parallel_mode =
            query_options.__isset.tablet_internal_parallel_mode ? query_options.tablet_internal_parallel_mode
                                                                : TTabletInternalParallelMode::type::AUTO;

    // Set up plan
    _fragment_ctx->move_tplan(*const_cast<TPlan*>(&fragment.plan));
    RETURN_IF_ERROR(
            ExecNode::create_tree(runtime_state, obj_pool, _fragment_ctx->tplan(), desc_tbl, &_fragment_ctx->plan()));
    ExecNode* plan = _fragment_ctx->plan();
    plan->push_down_join_runtime_filter_recursively(runtime_state);
    std::vector<TupleSlotMapping> empty_mappings;
    plan->push_down_tuple_slot_mappings(runtime_state, empty_mappings);
    runtime_state->set_fragment_root_id(plan->id());

    // Set senders of exchange nodes before pipeline build
    std::vector<ExecNode*> exch_nodes;
    plan->collect_nodes(TPlanNodeType::EXCHANGE_NODE, &exch_nodes);
    for (auto* exch_node : exch_nodes) {
        int num_senders = FindWithDefault(params.per_exch_num_senders, exch_node->id(), 0);
        down_cast<ExchangeNode*>(exch_node)->set_num_senders(num_senders);
    }

    // set scan ranges
    std::vector<ExecNode*> scan_nodes;
    plan->collect_scan_nodes(&scan_nodes);

    MorselQueueFactoryMap& morsel_queue_factories = _fragment_ctx->morsel_queue_factories();

    // If spill is turned on, then query cache will be turned off automatically
    // TODO: Fix
    if (fragment.__isset.cache_param && !runtime_state->enable_spill()) {
        auto const& tcache_param = fragment.cache_param;
        auto& cache_param = _fragment_ctx->cache_param();
        cache_param.plan_node_id = tcache_param.id;
        cache_param.digest = tcache_param.digest;
        cache_param.force_populate = tcache_param.force_populate;
        cache_param.entry_max_bytes = tcache_param.entry_max_bytes;
        cache_param.entry_max_rows = tcache_param.entry_max_rows;
        for (auto& [slot, remapped_slot] : tcache_param.slot_remapping) {
            cache_param.slot_remapping[slot] = remapped_slot;
            cache_param.reverse_slot_remapping[remapped_slot] = slot;
        }
        cache_param.can_use_multiversion = tcache_param.can_use_multiversion;
        cache_param.keys_type = tcache_param.keys_type;
        if (tcache_param.__isset.cached_plan_node_ids) {
            cache_param.cached_plan_node_ids.insert(tcache_param.cached_plan_node_ids.begin(),
                                                    tcache_param.cached_plan_node_ids.end());
        }
        _fragment_ctx->set_enable_cache(true);
    }

    for (auto& i : scan_nodes) {
        auto* scan_node = down_cast<ScanNode*>(i);
        const std::vector<TScanRangeParams>& scan_ranges = request.scan_ranges_of_node(scan_node->id());
        const auto& scan_ranges_per_driver_seq = request.per_driver_seq_scan_ranges_of_node(scan_node->id());

        // num_lanes ranges in [1,16] in default 4.
        _fragment_ctx->cache_param().num_lanes = std::min(16, std::max(1, config::query_cache_num_lanes_per_driver));

        if (scan_ranges_per_driver_seq.empty()) {
            _fragment_ctx->set_enable_cache(false);
        }

        bool should_compute_cache_key_prefix = _fragment_ctx->enable_cache() &&
                                               _fragment_ctx->cache_param().cached_plan_node_ids.count(scan_node->id());
        if (should_compute_cache_key_prefix) {
            for (auto& [driver_seq, scan_ranges] : scan_ranges_per_driver_seq) {
                for (auto& scan_range : scan_ranges) {
                    if (!scan_range.scan_range.__isset.internal_scan_range) {
                        continue;
                    }
                    const auto& tcache_param = fragment.cache_param;
                    auto& internal_scan_range = scan_range.scan_range.internal_scan_range;
                    auto tablet_id = internal_scan_range.tablet_id;
                    auto partition_id = internal_scan_range.partition_id;
                    if (!tcache_param.region_map.count(partition_id)) {
                        continue;
                    }
                    const auto& region = tcache_param.region_map.at(partition_id);
                    std::string cache_prefix_key;
                    cache_prefix_key.reserve(sizeof(partition_id) + region.size() + sizeof(tablet_id));
                    cache_prefix_key.insert(cache_prefix_key.end(), (uint8_t*)&partition_id,
                                            ((uint8_t*)&partition_id) + sizeof(partition_id));
                    cache_prefix_key.insert(cache_prefix_key.end(), region.begin(), region.end());
                    cache_prefix_key.insert(cache_prefix_key.end(), (uint8_t*)&tablet_id,
                                            ((uint8_t*)&tablet_id) + sizeof(tablet_id));
                    _fragment_ctx->cache_param().cache_key_prefixes[tablet_id] = std::move(cache_prefix_key);
                }
            }
        }

        // TODO (by satanson): shared_scan mechanism conflicts with per-tablet computation that is required for query
        //  cache, so it is turned off at present, it would be solved in the future.
        if (_fragment_ctx->enable_cache()) {
            enable_shared_scan = false;
        }

        ASSIGN_OR_RETURN(auto morsel_queue_factory,
                         scan_node->convert_scan_range_to_morsel_queue_factory(
                                 scan_ranges, scan_ranges_per_driver_seq, scan_node->id(), dop,
                                 enable_tablet_internal_parallel, tablet_internal_parallel_mode));
        scan_node->enable_shared_scan(enable_shared_scan && morsel_queue_factory->is_shared());
        morsel_queue_factories.emplace(scan_node->id(), std::move(morsel_queue_factory));
    }

    int64_t logical_scan_limit = 0;
    int64_t physical_scan_limit = 0;
    for (auto& i : scan_nodes) {
        auto* scan_node = down_cast<ScanNode*>(i);
        if (scan_node->limit() > 0) {
            // The upper bound of records we actually will scan is `limit * dop * io_parallelism`.
            // For SQL like: select * from xxx limit 5, the underlying scan_limit should be 5 * parallelism
            // Otherwise this SQL would exceed the bigquery_rows_limit due to underlying IO parallelization.
            // Some chunk sources scan `chunk_size` rows at a time, so normalize `limit` to be rounded up to `chunk_size`.
            logical_scan_limit += scan_node->limit();
            int64_t normalized_limit = (scan_node->limit() + chunk_size - 1) / chunk_size * chunk_size;
            physical_scan_limit += normalized_limit * dop * scan_node->io_tasks_per_scan_operator();
        } else {
            // Not sure how many rows will be scan.
            logical_scan_limit = -1;
            break;
        }
    }

    if (_wg && _wg->big_query_scan_rows_limit() > 0) {
        if (logical_scan_limit >= 0 && logical_scan_limit <= _wg->big_query_scan_rows_limit()) {
            _query_ctx->set_scan_limit(std::max(_wg->big_query_scan_rows_limit(), physical_scan_limit));
        } else {
            _query_ctx->set_scan_limit(_wg->big_query_scan_rows_limit());
        }
    }

    return Status::OK();
}

Status FragmentExecutor::_prepare_stream_load_pipe(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) {
    const TExecPlanFragmentParams& unique_request = request.unique();
    if (!unique_request.params.__isset.node_to_per_driver_seq_scan_ranges) {
        return Status::OK();
    }
    const auto& scan_range_map = unique_request.params.node_to_per_driver_seq_scan_ranges;
    if (scan_range_map.size() == 0) {
        return Status::OK();
    }
    auto iter = scan_range_map.begin();
    if (iter->second.size() == 0) {
        return Status::OK();
    }
    auto iter2 = iter->second.begin();
    if (iter2->second.size() == 0) {
        return Status::OK();
    }
    if (!iter2->second[0].scan_range.__isset.broker_scan_range) {
        return Status::OK();
    }
    if (!iter2->second[0].scan_range.broker_scan_range.__isset.channel_id) {
        return Status::OK();
    }
    std::vector<StreamLoadContext*> stream_load_contexts;
    for (; iter != scan_range_map.end(); iter++) {
        for (; iter2 != iter->second.end(); iter2++) {
            for (const auto& scan_range : iter2->second) {
                const TBrokerScanRange& broker_scan_range = scan_range.scan_range.broker_scan_range;
                int channel_id = broker_scan_range.channel_id;
                const string& label = broker_scan_range.params.label;
                const string& db_name = broker_scan_range.params.db_name;
                const string& table_name = broker_scan_range.params.table_name;
                TFileFormatType::type format = broker_scan_range.ranges[0].format_type;
                TUniqueId load_id = broker_scan_range.ranges[0].load_id;
                long txn_id = broker_scan_range.params.txn_id;
                StreamLoadContext* ctx = nullptr;
                RETURN_IF_ERROR(exec_env->stream_context_mgr()->create_channel_context(
                        exec_env, label, channel_id, db_name, table_name, format, ctx, load_id, txn_id));
                DeferOp op([&] {
                    if (ctx->unref()) {
                        delete ctx;
                    }
                });
                RETURN_IF_ERROR(exec_env->stream_context_mgr()->put_channel_context(label, channel_id, ctx));
                stream_load_contexts.push_back(ctx);
            }
        }
    }
    _fragment_ctx->set_stream_load_contexts(stream_load_contexts);
    return Status::OK();
}

void create_adaptive_group_initialize_events(RuntimeState* state, PipelineGroupMap&& unready_pipeline_groups) {
    if (unready_pipeline_groups.empty()) {
        return;
    }

    auto* driver_executor = state->exec_env()->wg_driver_executor();
    for (auto& [leader_source_op, pipelines] : unready_pipeline_groups) {
        EventPtr group_initialize_event =
                Event::create_collect_stats_source_initialize_event(driver_executor, std::move(pipelines));

        if (auto blocking_event = leader_source_op->adaptive_blocking_event(); blocking_event != nullptr) {
            group_initialize_event->add_dependency(blocking_event.get());
        }
        for (const auto* dependency_pipeline : leader_source_op->group_dependent_pipelines()) {
            group_initialize_event->add_dependency(dependency_pipeline->pipeline_event());
        }

        leader_source_op->set_group_initialize_event(std::move(group_initialize_event));
    }
}

Status FragmentExecutor::_prepare_pipeline_driver(ExecEnv* exec_env, const UnifiedExecPlanFragmentParams& request) {
    const auto degree_of_parallelism = _calc_dop(exec_env, request);
    const auto& fragment = request.common().fragment;
    const auto& params = request.common().params;
    auto is_stream_pipeline = request.is_stream_pipeline();
    ExecNode* plan = _fragment_ctx->plan();

    Drivers drivers;
    MorselQueueFactoryMap& morsel_queue_factories = _fragment_ctx->morsel_queue_factories();
    auto* runtime_state = _fragment_ctx->runtime_state();
    const auto& pipelines = _fragment_ctx->pipelines();
    size_t sink_dop = _calc_sink_dop(ExecEnv::GetInstance(), request);

    // Build pipelines
    PipelineBuilderContext context(_fragment_ctx.get(), degree_of_parallelism, sink_dop, is_stream_pipeline);
    PipelineBuilder builder(context);
    auto exec_ops = builder.decompose_exec_node_to_pipeline(*_fragment_ctx, plan);

    // Set up sink if required
    std::unique_ptr<DataSink> datasink;
    if (request.isset_output_sink()) {
        const auto& tsink = request.output_sink();
        if (tsink.type == TDataSinkType::RESULT_SINK || tsink.type == TDataSinkType::OLAP_TABLE_SINK ||
            tsink.type == TDataSinkType::MEMORY_SCRATCH_SINK || tsink.type == TDataSinkType::ICEBERG_TABLE_SINK ||
            tsink.type == TDataSinkType::HIVE_TABLE_SINK || tsink.type == TDataSinkType::EXPORT_SINK ||
            tsink.type == TDataSinkType::BLACKHOLE_TABLE_SINK || tsink.type == TDataSinkType::DICTIONARY_CACHE_SINK) {
            _query_ctx->set_final_sink();
        }
        RETURN_IF_ERROR(DataSink::create_data_sink(runtime_state, tsink, fragment.output_exprs, params,
                                                   request.sender_id(), plan->row_desc(), &datasink));
        RETURN_IF_ERROR(datasink->decompose_data_sink_to_pipeline(&context, runtime_state, std::move(exec_ops), request,
                                                                  tsink, fragment.output_exprs));
    }
    _fragment_ctx->set_data_sink(std::move(datasink));

    _fragment_ctx->set_pipelines(builder.build());

    RETURN_IF_ERROR(_fragment_ctx->prepare_all_pipelines());

    // Set morsel_queue_factory to pipeline.
    for (const auto& pipeline : pipelines) {
        if (pipeline->source_operator_factory()->with_morsels()) {
            auto source_id = pipeline->get_op_factories()[0]->plan_node_id();
            DCHECK(morsel_queue_factories.count(source_id));
            auto& morsel_queue_factory = morsel_queue_factories[source_id];

            pipeline->source_operator_factory()->set_morsel_queue_factory(morsel_queue_factory.get());
        }
    }

    PipelineGroupMap unready_pipeline_groups;
    for (const auto& pipeline : pipelines) {
        auto* source_op = pipeline->source_operator_factory();
        if (!source_op->is_adaptive_group_initial_active()) {
            auto* group_leader_source_op = source_op->group_leader();
            unready_pipeline_groups[group_leader_source_op].emplace_back(pipeline.get());
            continue;
        }

        pipeline->instantiate_drivers(runtime_state);
    }

    if (!unready_pipeline_groups.empty()) {
        create_adaptive_group_initialize_events(runtime_state, std::move(unready_pipeline_groups));
    }

    // Acquire driver token to avoid overload
    ASSIGN_OR_RETURN(auto driver_token, exec_env->driver_limiter()->try_acquire(_fragment_ctx->total_dop()));
    _fragment_ctx->set_driver_token(std::move(driver_token));

    return Status::OK();
}

Status FragmentExecutor::_prepare_global_dict(const UnifiedExecPlanFragmentParams& request) {
    const auto& fragment = request.common().fragment;
    // Set up global dict
    auto* runtime_state = _fragment_ctx->runtime_state();
    if (fragment.__isset.query_global_dicts) {
        RETURN_IF_ERROR(runtime_state->init_query_global_dict(fragment.query_global_dicts));
    }

    if (fragment.__isset.query_global_dicts && fragment.__isset.query_global_dict_exprs) {
        RETURN_IF_ERROR(runtime_state->init_query_global_dict_exprs(fragment.query_global_dict_exprs));
    }

    if (fragment.__isset.load_global_dicts) {
        RETURN_IF_ERROR(runtime_state->init_load_global_dict(fragment.load_global_dicts));
    }
    return Status::OK();
}

Status FragmentExecutor::prepare(ExecEnv* exec_env, const TExecPlanFragmentParams& common_request,
                                 const TExecPlanFragmentParams& unique_request) {
    DCHECK(common_request.__isset.desc_tbl);
    DCHECK(common_request.__isset.fragment);

    UnifiedExecPlanFragmentParams request(common_request, unique_request);

    bool prepare_success = false;
    struct {
        int64_t prepare_time = 0;
        int64_t prepare_query_ctx_time = 0;
        int64_t prepare_fragment_ctx_time = 0;
        int64_t prepare_runtime_state_time = 0;
        int64_t prepare_pipeline_driver_time = 0;

        int64_t process_mem_bytes = GlobalEnv::GetInstance()->process_mem_tracker()->consumption();
        size_t num_process_drivers = ExecEnv::GetInstance()->driver_limiter()->num_total_drivers();
    } profiler;

    DeferOp defer([this, &request, &prepare_success, &profiler]() {
        if (prepare_success) {
            auto fragment_ctx = _query_ctx->fragment_mgr()->get(request.fragment_instance_id());
            auto* profile = fragment_ctx->runtime_state()->runtime_profile();

            auto* prepare_timer = ADD_TIMER(profile, "FragmentInstancePrepareTime");
            COUNTER_SET(prepare_timer, profiler.prepare_time);

            auto* prepare_query_ctx_timer =
                    ADD_CHILD_TIMER_THESHOLD(profile, "prepare-query-ctx", "FragmentInstancePrepareTime", 10_ms);
            COUNTER_SET(prepare_query_ctx_timer, profiler.prepare_query_ctx_time);

            auto* prepare_fragment_ctx_timer =
                    ADD_CHILD_TIMER_THESHOLD(profile, "prepare-fragment-ctx", "FragmentInstancePrepareTime", 10_ms);
            COUNTER_SET(prepare_fragment_ctx_timer, profiler.prepare_fragment_ctx_time);

            auto* prepare_runtime_state_timer =
                    ADD_CHILD_TIMER_THESHOLD(profile, "prepare-runtime-state", "FragmentInstancePrepareTime", 10_ms);
            COUNTER_SET(prepare_runtime_state_timer, profiler.prepare_runtime_state_time);

            auto* prepare_pipeline_driver_timer = ADD_CHILD_TIMER_THESHOLD(profile, "prepare-pipeline-driver-factory",
                                                                           "FragmentInstancePrepareTime", 10_ms);
            COUNTER_SET(prepare_pipeline_driver_timer, profiler.prepare_pipeline_driver_time);

            auto* process_mem_counter = ADD_COUNTER(profile, "InitialProcessMem", TUnit::BYTES);
            COUNTER_SET(process_mem_counter, profiler.process_mem_bytes);
            auto* num_process_drivers_counter = ADD_COUNTER(profile, "InitialProcessDriverCount", TUnit::UNIT);
            COUNTER_SET(num_process_drivers_counter, static_cast<int64_t>(profiler.num_process_drivers));

            VLOG_QUERY << "Prepare fragment succeed: query_id=" << print_id(request.common().params.query_id)
                       << " fragment_instance_id=" << print_id(request.fragment_instance_id())
                       << " is_stream_pipeline=" << request.is_stream_pipeline()
                       << " backend_num=" << request.backend_num()
                       << " fragment plan=" << fragment_ctx->plan()->debug_string();
        } else {
            _fail_cleanup(prepare_success);
            LOG(WARNING) << "Prepare fragment failed: " << print_id(request.common().params.query_id)
                         << " fragment_instance_id=" << print_id(request.fragment_instance_id())
                         << " is_stream_pipeline=" << request.is_stream_pipeline()
                         << " backend_num=" << request.backend_num() << " fragment= " << request.common().fragment;
        }
    });

    SCOPED_RAW_TIMER(&profiler.prepare_time);
    RETURN_IF_ERROR(
            GlobalEnv::GetInstance()->query_pool_mem_tracker()->check_mem_limit("Start execute plan fragment."));
    {
        SCOPED_RAW_TIMER(&profiler.prepare_query_ctx_time);
        RETURN_IF_ERROR(_prepare_query_ctx(exec_env, request));
    }
    {
        SCOPED_RAW_TIMER(&profiler.prepare_fragment_ctx_time);
        RETURN_IF_ERROR(_prepare_fragment_ctx(request));
    }
    {
        SCOPED_RAW_TIMER(&profiler.prepare_runtime_state_time);
        RETURN_IF_ERROR(_prepare_workgroup(request));
        RETURN_IF_ERROR(_prepare_runtime_state(exec_env, request));

        auto mem_tracker = _fragment_ctx->runtime_state()->instance_mem_tracker();
        SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(mem_tracker);

        RETURN_IF_ERROR(_prepare_global_dict(request));
        RETURN_IF_ERROR(_prepare_exec_plan(exec_env, request));
    }
    {
        SCOPED_RAW_TIMER(&profiler.prepare_pipeline_driver_time);

        auto mem_tracker = _fragment_ctx->runtime_state()->instance_mem_tracker();
        SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(mem_tracker);

        RETURN_IF_ERROR(_prepare_pipeline_driver(exec_env, request));
        RETURN_IF_ERROR(_prepare_stream_load_pipe(exec_env, request));
    }

    RETURN_IF_ERROR(_query_ctx->fragment_mgr()->register_ctx(request.fragment_instance_id(), _fragment_ctx));
    _query_ctx->mark_prepared();
    prepare_success = true;

    return Status::OK();
}

Status FragmentExecutor::execute(ExecEnv* exec_env) {
    bool prepare_success = false;
    DeferOp defer([this, &prepare_success]() {
        if (!prepare_success) {
            _fail_cleanup(true);
        }
    });

    auto* profile = _fragment_ctx->runtime_state()->runtime_profile();
    auto* prepare_instance_timer = ADD_TIMER(profile, "FragmentInstancePrepareTime");
    auto* prepare_driver_timer =
            ADD_CHILD_TIMER_THESHOLD(profile, "prepare-pipeline-driver", "FragmentInstancePrepareTime", 10_ms);

    auto iterate_active_drivers = [fragment_ctx = _fragment_ctx.get()](auto call) {
        using Func = decltype(call);
        static_assert(DriverPtrCallable<Func>, "Function must be callable with DriverPtr and return void or Status.");
        using ReturnType = std::invoke_result_t<Func, const DriverPtr&>;

        for (const auto& pipeline : fragment_ctx->pipelines()) {
            auto* source_op = pipeline->source_operator_factory();
            if (!source_op->is_adaptive_group_initial_active()) {
                continue;
            }
            for (const auto& driver : pipeline->drivers()) {
                if constexpr (std::is_same_v<ReturnType, Status>) {
                    RETURN_IF_ERROR(call(driver));
                } else {
                    call(driver);
                }
            }
        }
        if constexpr (std::is_same_v<ReturnType, Status>) {
            return Status::OK();
        }
    };

    {
        SCOPED_TIMER(prepare_instance_timer);
        SCOPED_TIMER(prepare_driver_timer);
        RETURN_IF_ERROR(iterate_active_drivers(
                [state = _fragment_ctx->runtime_state()](const DriverPtr& driver) { return driver->prepare(state); }));
    }
    prepare_success = true;

    DCHECK(_fragment_ctx->enable_resource_group());
    auto* executor = exec_env->wg_driver_executor();
    iterate_active_drivers([executor](const DriverPtr& driver) { executor->submit(driver.get()); });

    return Status::OK();
}

void FragmentExecutor::_fail_cleanup(bool fragment_has_registed) {
    if (_query_ctx) {
        if (_fragment_ctx) {
            if (fragment_has_registed) {
                _query_ctx->fragment_mgr()->unregister(_fragment_ctx->fragment_instance_id());
            }
            _fragment_ctx->destroy_pass_through_chunk_buffer();
            _fragment_ctx.reset();
        }
        _query_ctx->count_down_fragments();
    }
}
} // namespace starrocks::pipeline
