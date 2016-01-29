#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TCypressManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Period between Cypress access statistics commits.
    TDuration StatisticsFlushPeriod;

    //! Maximum number of children map and list nodes are allowed to contain.
    //! NB: Changing these values will invalidate all changelogs!
    int MaxNodeChildCount;

    //! Maximum allowed length of string nodes.
    //! NB: Changing these values will invalidate all changelogs!
    int MaxStringNodeLength;

    //! Maximum allowed size of custom attributes for objects (transactions, Cypress nodes etc).
    //! This limit concerns the binary YSON representation of attributes.
    //! NB: Changing these values will invalidate all changelogs!
    int MaxAttributeSize;

    // NB: Changing these values will invalidate all changelogs!
    int DefaultFileReplicationFactor;
    int DefaultTableReplicationFactor;
    int DefaultJournalReplicationFactor;
    int DefaultJournalReadQuorum;
    int DefaultJournalWriteQuorum;

    TCypressManagerConfig()
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
            .Default(16 * 1024 * 1024);

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

        RegisterValidator([&] () {
            if (DefaultJournalReadQuorum + DefaultJournalWriteQuorum < DefaultJournalReplicationFactor + 1) {
                THROW_ERROR_EXCEPTION("Default read/write quorums are not safe: "
                    "default_journal_read_quorum + default_journal_write_quorum < default_journal_replication_factor + 1");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TCypressManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
