#include "config.h"

#include <yt/yt/ytlib/journal_client/helpers.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NHydra {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void TPeerConnectionConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cell_id", &TThis::CellId)
        .Default();

    registrar.Preprocessor([] (TThis* config) {
        // Query all peers in parallel.
        config->MaxConcurrentDiscoverRequests = std::numeric_limits<int>::max();
    });

    registrar.Postprocessor([] (TThis* config) {
        if (!config->CellId) {
            THROW_ERROR_EXCEPTION("\"cell_id\" cannot be equal to %v",
                NullCellId);
        }
    });
}

TRemoteSnapshotStoreOptions::TRemoteSnapshotStoreOptions()
{
    RegisterParameter("snapshot_replication_factor", SnapshotReplicationFactor)
        .GreaterThan(0)
        .InRange(1, NChunkClient::MaxReplicationFactor)
        .Default(3);
    RegisterParameter("snapshot_compression_codec", SnapshotCompressionCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("snapshot_account", SnapshotAccount)
        .NonEmpty();
    RegisterParameter("snapshot_primary_medium", SnapshotPrimaryMedium)
        .Default(NChunkClient::DefaultStoreMediumName);
    RegisterParameter("snapshot_erasure_codec", SnapshotErasureCodec)
        .Default(NErasure::ECodec::None);
    RegisterParameter("snapshot_acl", SnapshotAcl)
        .Default(BuildYsonNodeFluently()
            .BeginList()
            .EndList()->AsList());
}

TRemoteChangelogStoreOptions::TRemoteChangelogStoreOptions()
{
    RegisterParameter("changelog_erasure_codec", ChangelogErasureCodec)
        .Default(NErasure::ECodec::None);
    RegisterParameter("changelog_replication_factor", ChangelogReplicationFactor)
        .GreaterThan(0)
        .InRange(1, NChunkClient::MaxReplicationFactor)
        .Default(3);
    RegisterParameter("changelog_read_quorum", ChangelogReadQuorum)
        .GreaterThan(0)
        .InRange(1, NChunkClient::MaxReplicationFactor)
        .Default(2);
    RegisterParameter("changelog_write_quorum", ChangelogWriteQuorum)
        .GreaterThan(0)
        .InRange(1, NChunkClient::MaxReplicationFactor)
        .Default(2);
    RegisterParameter("enable_changelog_multiplexing", EnableChangelogMultiplexing)
        .Default(true);
    RegisterParameter("enable_changelog_chunk_preallocation", EnableChangelogChunkPreallocation)
        .Default(false);
    RegisterParameter("changelog_replica_lag_limit", ChangelogReplicaLagLimit)
        .Default(NJournalClient::DefaultReplicaLagLimit);
    RegisterParameter("changelog_account", ChangelogAccount)
        .NonEmpty();
    RegisterParameter("changelog_primary_medium", ChangelogPrimaryMedium)
        .Default(NChunkClient::DefaultStoreMediumName);
    RegisterParameter("changelog_acl", ChangelogAcl)
        .Default(BuildYsonNodeFluently()
            .BeginList()
            .EndList()->AsList());

    RegisterPostprocessor([&] () {
        NJournalClient::ValidateJournalAttributes(
            ChangelogErasureCodec,
            ChangelogReplicationFactor,
            ChangelogReadQuorum,
            ChangelogWriteQuorum);
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
