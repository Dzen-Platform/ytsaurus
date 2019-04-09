#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TDynamicCypressManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Period between Cypress access statistics commits.
    TDuration StatisticsFlushPeriod;

    //! Maximum number of children map and list nodes are allowed to contain.
    int MaxNodeChildCount;

    //! Maximum allowed length of string nodes.
    int MaxStringNodeLength;

    //! Maximum allowed size of custom attributes for objects (transactions, Cypress nodes etc).
    //! This limit concerns the binary YSON representation of attributes.
    int MaxAttributeSize;

    //! Maximum allowed length of keys in map nodes.
    int MaxMapNodeKeyLength;

    //! Default replication factors.
    int DefaultFileReplicationFactor;
    int DefaultTableReplicationFactor;
    int DefaultJournalReplicationFactor;
    int DefaultJournalReadQuorum;
    int DefaultJournalWriteQuorum;

    TDuration ExpirationCheckPeriod;
    int MaxExpiredNodesRemovalsPerCommit;
    TDuration ExpirationBackoffTime;

    // COMPAT(ignat)
    //! Forbids performing set inside Cypress.
    bool ForbidSetCommand;

    // COMPAT(shakurov)
    //! Controls if unlock command is enabled.
    bool EnableUnlockCommand;

    TDynamicCypressManagerConfig()
    {
        RegisterParameter("statistics_flush_period", StatisticsFlushPeriod)
            .GreaterThan(TDuration())
            .Default(TDuration::Seconds(1));
        RegisterParameter("max_node_child_count", MaxNodeChildCount)
            .GreaterThan(20)
            .Default(50000);
        RegisterParameter("max_string_node_length", MaxStringNodeLength)
            .GreaterThan(256)
            .Default(65536);
        RegisterParameter("max_attribute_size", MaxAttributeSize)
            .GreaterThan(256)
            .Default(16_MB);
        RegisterParameter("max_map_node_key_length", MaxMapNodeKeyLength)
            .GreaterThan(256)
            .Default(4096);

        RegisterParameter("default_file_replication_factor", DefaultFileReplicationFactor)
            .Default(3)
            .InRange(NChunkClient::MinReplicationFactor, NChunkClient::MaxReplicationFactor);
        RegisterParameter("default_table_replication_factor", DefaultTableReplicationFactor)
            .Default(3)
            .InRange(NChunkClient::MinReplicationFactor, NChunkClient::MaxReplicationFactor);
        RegisterParameter("default_journal_replication_factor", DefaultJournalReplicationFactor)
            .Default(3)
            .InRange(NChunkClient::MinReplicationFactor, NChunkClient::MaxReplicationFactor);
        RegisterParameter("default_journal_read_quorum", DefaultJournalReadQuorum)
            .Default(2)
            .InRange(NChunkClient::MinReplicationFactor, NChunkClient::MaxReplicationFactor);
        RegisterParameter("default_journal_write_quorum", DefaultJournalWriteQuorum)
            .Default(2)
            .InRange(NChunkClient::MinReplicationFactor, NChunkClient::MaxReplicationFactor);

        RegisterParameter("expiration_check_period", ExpirationCheckPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("max_expired_nodes_removals_per_commit", MaxExpiredNodesRemovalsPerCommit)
            .Default(1000);
        RegisterParameter("expiration_backoff_time", ExpirationBackoffTime)
            .Default(TDuration::Seconds(10));

        RegisterParameter("forbid_set_command", ForbidSetCommand)
            .Default(false);
        RegisterParameter("enable_unlock_command", EnableUnlockCommand)
            .Default(false);

        RegisterPostprocessor([&] () {
            if (DefaultJournalReadQuorum + DefaultJournalWriteQuorum < DefaultJournalReplicationFactor + 1) {
                THROW_ERROR_EXCEPTION("Default read/write quorums are not safe: "
                    "default_journal_read_quorum + default_journal_write_quorum < default_journal_replication_factor + 1");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicCypressManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
