#pragma once

#include "private.h"

#include <core/rpc/public.h>

#include <core/misc/ref.h>

#include <core/concurrency/thread_affinity.h>

#include <core/logging/log.h>

#include <ytlib/election/public.h>

#include <ytlib/hydra/version.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TRecoveryBase
    : public TRefCounted
{
protected:
    TRecoveryBase(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IChangelogStorePtr changelogStore,
        ISnapshotStorePtr snapshotStore,
        NRpc::TResponseKeeperPtr responseKeeper,
        TEpochContext* epochContext);

    //! Must be derived by the inheritors to control the recovery behavior.
    virtual bool IsLeader() const = 0;

    //! Recovers to the desired state by first loading an appropriate snapshot
    //! and then applying changelogs, if necessary.
    void RecoverToVersion(TVersion targetVersion);


    const TDistributedHydraManagerConfigPtr Config_;
    const NElection::TCellManagerPtr CellManager_;
    const TDecoratedAutomatonPtr DecoratedAutomaton_;
    const IChangelogStorePtr ChangelogStore_;
    const ISnapshotStorePtr SnapshotStore_;
    const NRpc::TResponseKeeperPtr ResponseKeeper_;
    TEpochContext* const EpochContext_;

    TVersion SyncVersion_;

    NLogging::TLogger Logger;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

private:
    //! Synchronizes the changelog at follower with the leader, i.e.
    //! downloads missing records or truncates redundant ones.
    void SyncChangelog(IChangelogPtr changelog, int changelogId);

    //! Applies records from a given changes up to a given one.
    /*!
     *  The current segment id should match that of #changeLog.
     *  The methods ensures that no mutation is applied twice.
     */
    void ReplayChangelog(IChangelogPtr changelog, int changelogId, int targetRecordId);

};

////////////////////////////////////////////////////////////////////////////////

//! Drives the leader recovery.
/*!
 *  \note
 *  Thread affinity: any
 */
class TLeaderRecovery
    : public TRecoveryBase
{
public:
    TLeaderRecovery(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IChangelogStorePtr changelogStore,
        ISnapshotStorePtr snapshotStore,
        NRpc::TResponseKeeperPtr responseKeeper,
        TEpochContext* epochContext);

    //! Performs leader recovery up to a given version.
    TFuture<void> Run(TVersion targetVersion);

private:
    void DoRun(TVersion targetVersion);

    virtual bool IsLeader() const;

};

DEFINE_REFCOUNTED_TYPE(TLeaderRecovery)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EPostponedMutationType,
    (Mutation)
    (ChangelogRotation)
);

//! Drives the follower recovery.
/*!
 *  \note
 *  Thread affinity: any
 */
class TFollowerRecovery
    : public TRecoveryBase
{
public:
    TFollowerRecovery(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IChangelogStorePtr changelogStore,
        ISnapshotStorePtr snapshotStore,
        NRpc::TResponseKeeperPtr responseKeeper,
        TEpochContext* epochContext,
        TVersion syncVersion);

    //! Performs follower recovery bringing the follower up-to-date and synchronized with the leader.
    TFuture<void> Run();

    //! Postpones an incoming request for changelog rotation.
    void PostponeChangelogRotation(TVersion version);

    //! Postpones incoming mutations.
    void PostponeMutations(
        TVersion version,
        const std::vector<TSharedRef>& recordsData);

    //! Notifies the recovery process about the latest committed version available at leader.
    void SetCommittedVersion(TVersion version);

private:
    struct TPostponedMutation
    {
        using EType = EPostponedMutationType;

        EType Type;
        TSharedRef RecordData;

        static TPostponedMutation CreateMutation(const TSharedRef& recordData)
        {
            return TPostponedMutation(EType::Mutation, recordData);
        }

        static TPostponedMutation CreateChangelogRotation()
        {
            return TPostponedMutation(EType::ChangelogRotation, TSharedRef());
        }

        TPostponedMutation(EType type, const TSharedRef& recordData)
            : Type(type)
            , RecordData(recordData)
        { }

    };

    typedef std::vector<TPostponedMutation> TPostponedMutations;

    TSpinLock SpinLock_;
    TPostponedMutations PostponedMutations_;
    TVersion PostponedVersion_;
    TVersion CommittedVersion_;

    void DoRun();

    virtual bool IsLeader() const;

};

DEFINE_REFCOUNTED_TYPE(TFollowerRecovery)

//////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
