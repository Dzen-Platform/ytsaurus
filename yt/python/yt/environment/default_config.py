import yt.yson as yson


# TODO(babenko): drop settings mirrored in get_dynamic_master_config below
def get_master_config():
    return yson.loads(
b"""
{
    enable_provision_lock = %false;

    timestamp_provider = {
        soft_backoff_time = 100;
        hard_backoff_time = 100;
        update_period = 500;
    };

    changelogs = {
        flush_period = 10;
        enable_sync = %false;
        io_engine = {
            enable_sync = %false;
        }
    };

    cell_directory = {
        soft_backoff_time = 100;
        hard_backoff_time = 100;
    };

    object_service = {
        enable_local_read_executor = %true;
    };

    timestamp_manager = {
        commit_advance = 2000;
        request_backoff_time = 100;
        calibration_period = 200;
    };

    hive_manager = {
        ping_period = 1000;
        idle_post_period = 1000;
    };

    cell_directory_synchronizer = {
        sync_period = 500;
        sync_period_splay = 100;
    };
}
""")

def get_dynamic_master_config():
    return yson.loads(
b"""
{
    chunk_manager = {
        chunk_refresh_delay = 300;
        chunk_refresh_period = 200;
    };

    node_tracker = {
        master_cache_manager = {
            update_period = 1000;
        };
        timestamp_provider_manager = {
            update_period = 1000;
        };
        full_node_states_gossip_period = 1000;
        total_node_statistics_update_period = 1000;
        use_new_heartbeats = %true;
    };

    object_manager = {
        gc_sweep_period = 10;
    };

    object_service = {
        process_sessions_period = 1;
        enable_local_read_executor = %true;
    };

    security_manager = {
        account_statistics_gossip_period = 200;
        request_rate_smoothing_period = 60000;
        account_master_memory_usage_update_period = 500;
        enable_delayed_membership_closure_recomputation = %false;
    };

    cypress_manager = {
        statistics_flush_period = 200;
        expiration_check_period = 200;
        expiration_backoff_time = 200;
        enable_unlock_command = %true;
    };

    multicell_manager = {
        cell_statistics_gossip_period = 200;
    };

    tablet_manager = {
        cell_scan_period = 100;
        accumulate_preload_pending_store_count_correctly = %true;
        increase_upload_replication_factor = %true;

        replicated_table_tracker = {
            check_period = 100;
            cluster_directory_synchronizer = {
                sync_period = 500;
            };
        };

        tablet_balancer = {
            enable_tablet_balancer = %false;
        };

        tablet_cell_decommissioner = {
            decommission_check_period = 100;
            orphans_check_period = 100;
        };

        multicell_gossip = {
            table_statistics_gossip_period = 100;
            tablet_cell_statistics_gossip_period = 100;
            tablet_cell_status_full_gossip_period = 100;
            tablet_cell_status_incremental_gossip_period = 100;
            bundle_resource_usage_gossip_period = 100;
        };

        enable_bulk_insert = %true;
        enable_hunks = %true;
    };
}
""")

def get_scheduler_config():
    return yson.loads(
b"""
{
    cluster_connection = {
        scheduler = {
            retry_backoff_time = 100;
        }
    };

    response_keeper = {
        enable_warmup = %false;
        expiration_time = 25000;
        warmup_time = 30000;
    };

    scheduler = {
        lock_transaction_timeout = 10000;
        operations_update_period = 500;
        fair_share_update_period = 500;
        watchers_update_period = 100;
        nodes_attributes_update_period = 100;
        scheduling_tag_filter_expire_timeout = 100;
        node_shard_exec_nodes_cache_update_period = 100;
        schedule_job_time_limit = 5000;
        exec_node_descriptors_update_period = 100;
        static_orchid_cache_update_period = 100;
        operation_to_agent_assignment_backoff = 100;
        orchid_keys_update_period = 100;

        min_needed_resources_update_period = 100;

        job_revival_abort_timeout = 2000;

        validate_node_tags_period = 100;
        parse_operation_attributes_batch_size = 2;

        operations_cleaner = {
            parse_operation_attributes_batch_size = 2;
        };
    };
}
""")

def get_clock_config():
    return yson.loads(
b"""
{
    timestamp_provider = {
        soft_backoff_time = 100;
        hard_backoff_time = 100;
        update_period = 500;
    };

    changelogs = {
        flush_period = 10;
        enable_sync = %false;
        io_engine = {
            enable_sync = %true;
        };
    };

    timestamp_manager = {
        commit_advance = 2000;
        request_backoff_time = 10;
        calibration_period = 10;
    };
}
""")

def get_timestamp_provider_config():
    return yson.loads(
b"""
{
    timestamp_provider = {
        soft_backoff_time = 100;
        hard_backoff_time = 100;
        update_period = 500;
    };
}
""")

def get_cell_balancer_config():
    return yson.loads(
b"""
{
    cluster_connection = { };

    election_manager = {
        lock_path = "//sys/cell_balancers/lock";
    };

    cell_balancer = {
        tablet_manager = {
            cell_scan_period = 100;
            tablet_cell_balancer = {
                enable_verbose_logging = %true;
            };
        };
    };
}
"""
)

def get_controller_agent_config():
    return yson.loads(
b"""
{
    node_directory_synchronizer = {
        sync_period = 100;
    };

    cluster_connection = {
        scheduler = {
            retry_backoff_time = 100;
        };
        node_directory_synchronizer = {
            sync_period = 100;
            expire_after_successful_update_time = 100;
            expire_after_failed_update_time = 100;
        };
    };

    controller_agent = {
        operations_update_period = 500;
        scheduling_tag_filter_expire_timeout = 100;
        safe_scheduler_online_time = 5000;

        static_orchid_cache_update_period = 300;

        controller_static_orchid_update_period = 0;

        operations_push_period = 10;
        operation_alerts_push_period = 100;
        suspicious_jobs_push_period = 100;

        config_update_period = 100;

        controller_exec_node_info_update_period = 100;

        exec_nodes_update_period = 100;

        environment = {
            TMPDIR = "$(SandboxPath)";
            PYTHON_EGG_CACHE = "$(SandboxPath)/.python-eggs";
            PYTHONUSERBASE = "$(SandboxPath)/.python-site-packages";
            PYTHONPATH = "$(SandboxPath)";
            HOME = "$(SandboxPath)";
        };

        testing_options = {
            enable_snapshot_cycle_after_materialization = %true;
        };

        enable_snapshot_loading = %true;

        snapshot_period = 100000000;
        snapshot_timeout = 5000;

        transactions_refresh_period = 500;

        max_archived_job_spec_count_per_operation = 10;

        operation_options = {
            spec_template = {
                max_failed_job_count = 10;
                locality_timeout = 100;
                intermediate_data_replication_factor = 1;
                enable_trace_logging = %true;
            }
        };
        sorted_merge_operation_options = {
            spec_template = {
                use_new_sorted_pool = %true;
            };
        };
        reduce_operation_options = {
            spec_template = {
                use_new_sorted_pool = %true;
            };
        };
        join_reduce_operation_options = {
            spec_template = {
                use_new_sorted_pool = %true;
            };
        };
        sort_operation_options = {
            spec_template = {
                use_new_sorted_pool = %true;
            };
        };
        map_reduce_operation_options = {
            spec_template = {
                use_new_sorted_pool = %true;
            };
        };

        enable_bulk_insert_for_everyone = %true;
        enable_heartbeats_from_nodes = %true;
    };
}
""")

def get_node_config():
    return yson.loads(
b"""
{
    orchid_cache_update_period = 0;

    master_cache_service = {
        capacity = 16777216;
        rate_limit = 100;
    };

    node_directory_synchronizer = {
        sync_period = 100;
    };

    dynamic_config_manager = {
        update_period = 50;
    };

    cluster_connection = {
        transaction_manager = {
            default_transaction_timeout = 3000;
        };

        scheduler = {
            retry_backoff_time = 100;
        };

        node_directory_synchronizer = {
            sync_period = 100;
            expire_after_successful_update_time = 100;
            expire_after_failed_update_time = 100;
        };

        enable_udf = %true;
    };

    data_node = {
        read_thread_count =  2;
        write_thread_count = 2;

        multiplexed_changelog = {
            flush_period = 10;
            enable_sync = %false;
            io_engine = {
                enable_sync = %false;
            };
        };
        high_latency_split_changelog = {
            flush_period = 10;
            enable_sync = %false;
            io_engine = {
                enable_sync = %false;
            };
        };
        low_latency_split_changelog = {
            flush_period = 10;
            enable_sync = %false;
            io_engine = {
                enable_sync = %false;
            };
        };

        incremental_heartbeat_period = 200;
        incremental_heartbeat_period_splay = 100;
        incremental_heartbeat_throttler = {
            limit = 100;
            period = 1000;
        };
        register_retry_period = 100;

        master_connector = {
            incremental_heartbeat_period = 200;
            incremental_heartbeat_period_splay = 50;
            job_heartbeat_period = 200;
            job_heartbeat_period_splay = 50;
        };

        chunk_meta_cache = {
            capacity = 1000000;
            shard_count = 1;
        };

        block_meta_cache = {
            capacity = 1000000;
            shard_count = 1;
        };

        blocks_ext_cache = {
            capacity = 1000000;
            shard_count = 1;
        };

        block_cache = {
            compressed_data = {
                capacity = 1000000;
                shard_count = 1;
            };
            uncompressed_data = {
                capacity = 1000000;
                shard_count = 1;
            };
        };

        p2p = {
            enabled = %false;
        };

        sync_directories_on_connect = %true;
    };

    exec_agent = {
        slot_manager = {
            job_environment = {
                type = simple;
                use_short_container_names = %true;
            };
        };

        node_directory_prepare_backoff_time = 100;

        scheduler_connector = {
            failed_heartbeat_backoff_start_time = 50;
            failed_heartbeat_backoff_max_time = 50;
            failed_heartbeat_backoff_multiplier = 1.0;
            heartbeat_period = 200;
        };

        controller_agent_connector = {
            running_job_sending_backoff = 0;
        };

        job_proxy_heartbeat_period = 200;

        job_controller = {
            total_confirmation_period = 5000;
            cpu_per_tablet_slot = 0.0;
        };

        master_connector = {
            heartbeat_period = 100;
            heartbeat_period_splay = 30;
        };
    };

    cellar_node = {
        master_connector = {
            heartbeat_period = 100;
            heartbeat_period_splay = 30;
        };
    };

    tablet_node = {
        slot_scan_period = 100;

        tablet_manager = {
            preload_backoff_time = 100;
            compaction_backoff_time = 100;
            flush_backoff_time = 100;
            replicator_soft_backoff_time = 100;
            replicator_hard_backoff_time = 100;
            tablet_cell_decommission_check_period = 100;
        };

        security_manager = {
            table_permission_cache = {
                expire_after_successful_update_time = 0;
                expire_after_failed_update_time = 0;
                refresh_time = 0;
            };
            resource_limits_cache = {
                expire_after_successful_update_time = 0;
                expire_after_failed_update_time = 0;
                refresh_time = 0;
            };
        };

        hive_manager = {
            ping_period = 1000;
            idle_post_period = 1000;
            rpc_timeout = 1000;
        };

        master_connector = {
            heartbeat_period = 100;
            heartbeat_period_splay = 30;
        };

        versioned_chunk_meta_cache = {
            capacity = 1000000;
            shard_count = 1;
        };
    };

    query_agent = {
        pool_weight_cache = {
            expire_after_successful_update_time = 0;
            expire_after_failed_update_time = 0;
            refresh_time = 0;
        };
    };

    master_connector = {
        heartbeat_period = 100;
        heartbeat_period_splay = 30;
    };

    use_new_heartbeats = %true;
}
""")

def get_chaos_node_config():
    return yson.loads(
b"""
{
    orchid_cache_update_period = 0;

    node_directory_synchronizer = {
        sync_period = 100;
    };

    dynamic_config_manager = {
        update_period = 50;
    };

    cluster_connection = {
        transaction_manager = {
            default_transaction_timeout = 3000;
        };

        scheduler = {
            retry_backoff_time = 100;
        };

        node_directory_synchronizer = {
            expire_after_successful_update_time = 100;
            expire_after_failed_update_time = 100;
        };
    };

    flavors = [
        "data";
        "exec";
        "tablet";
        "chaos";
    ];

    data_node = {
        block_cache = {
            compressed_data = {
                capacity = 0;
            };
            uncompressed_data = {
                capacity = 0;
            };
        };

        chunk_meta_cache = {
            capacity = 0;
        };
    };

    exec_agent = {
        job_controller = {
            resource_limits = {
                user_slots = 0;
                cpu = 0;
            };
        };

        node_directory_prepare_backoff_time = 100;
    };

    tablet_node = {
        resource_limits ={
            slots = 0;
        };

        versioned_chunk_meta_cache = {
            capacity = 0;
        };
    };

    chaos_node = {
        chaos_manager = {
            chaos_cell_synchronizer = {
                sync_period = 100;
            };
            era_commencing_period = 100;
        };
    };

    cellar_node = {
        master_connector = {
            heartbeat_period = 100;
            heartbeat_period_splay = 30;
        };

        cellar_manager = {
            cellars = {
                chaos = {
                    size = 4;
                };
            };
        };
    };

    master_connector = {
        heartbeat_period = 100;
        heartbeat_period_splay = 30;
    };

    use_new_heartbeats = %true;
}
""")

def get_master_cache_config():
    return yson.loads(
b"""
{
    cluster_connection = {
        scheduler = {
            retry_backoff_time = 100;
        };
        node_directory_synchronizer = {
            sync_period = 100;
            expire_after_successful_update_time = 100;
            expire_after_failed_update_time = 100;
        };
    };
}
"""
)

def get_dynamic_node_config():
    return yson.loads(
b"""
{
    "%true" = {
        config_annotation = "default";
        exec_agent = {
            abort_on_jobs_disabled = %true;
        };
    };
}
""")

def get_driver_config():
    return yson.loads(
b"""
{
    format_defaults = {
        structured = <
            format = text;
        > yson;
        tabular = <
            format = text;
        > yson
    };

    force_tracing = %true;

    enable_udf = %true;

    proxy_discovery_cache = {
        refresh_time = 1000;
        expire_after_successful_update_time = 1000;
        expire_after_failed_update_time = 1000;
    };
}
""")

def get_proxy_config():
    return yson.loads(
b"""
{
    port = -1;

    api = {
        disable_cors_check = %true;
    };

    auth = {
        enable_authentication = %false;
        blackbox_service = {};
        cypress_token_authenticator = {};
        blackbox_token_authenticator = {
            scope = "";
            enable_scope_check = %false;
        };
        blackbox_cookie_authenticator = {};
    };

    coordinator = {
        enable = %true;
        announce = %true;
        heartbeat_interval = 500;
        show_ports = %true;
    };

    dynamic_config_manager = {
        update_period = 100;
    };
}
""")

def get_watcher_config():
    return {
        "logs_rotate_max_part_count": 100,
        "logs_rotate_size": "1M",
        "logs_rotate_interval": 600,
    }

def get_queue_agent_config():
    return yson.loads(
b"""
{
    queue_agent = {
        root = "//sys/queue_agents";
    }
}
""")
