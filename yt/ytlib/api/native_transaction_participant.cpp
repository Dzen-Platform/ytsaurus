#include "native_transaction_participant.h"

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/transaction_participant.h>
#include <yt/ytlib/hive/transaction_participant_service_proxy.h>

#include <yt/ytlib/api/connection.h>

namespace NYT {
namespace NApi {

using namespace NHiveClient;
using namespace NTransactionClient;
using namespace NElection;

////////////////////////////////////////////////////////////////////////////////

class TNativeTransactionParticipant
    : public ITransactionParticipant
{
public:
    TNativeTransactionParticipant(
        TCellDirectoryPtr cellDirectory,
        ITimestampProviderPtr timestampProvider,
        const TCellId& cellId,
        const TTransactionParticipantOptions& options)
        : CellDirectory_(std::move(cellDirectory))
        , TimestampProvider_(std::move(timestampProvider))
        , CellId_(cellId)
        , Options_(options)
    { }

    virtual const TCellId& GetCellId() const override
    {
        return CellId_;
    }

    virtual const ITimestampProviderPtr& GetTimestampProvider() const override
    {
        return TimestampProvider_;
    }

    virtual bool IsValid() const override
    {
        return !CellDirectory_->IsCellUnregistered(CellId_);
    }

    virtual TFuture<void> PrepareTransaction(const TTransactionId& transactionId, TTimestamp prepareTimestamp) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqPrepareTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                auto req = proxy->PrepareTransaction();
                PrepareRequest(req);
                ToProto(req->mutable_transaction_id(), transactionId);
                req->set_prepare_timestamp(prepareTimestamp);
                return req;
            });
    }

    virtual TFuture<void> CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqCommitTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                auto req = proxy->CommitTransaction();
                PrepareRequest(req);
                ToProto(req->mutable_transaction_id(), transactionId);
                req->set_commit_timestamp(commitTimestamp);
                return req;
            });
    }

    virtual TFuture<void> AbortTransaction(const TTransactionId& transactionId) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqAbortTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                auto req = proxy->AbortTransaction();
                PrepareRequest(req);
                ToProto(req->mutable_transaction_id(), transactionId);
                return req;
            });
    }

private:
    const TCellDirectoryPtr CellDirectory_;
    const ITimestampProviderPtr TimestampProvider_;
    const TCellId CellId_;
    const TTransactionParticipantOptions Options_;


    template <class TRequest>
    TFuture<void> SendRequest(std::function<TIntrusivePtr<TRequest>(TTransactionParticipantServiceProxy*)> builder)
    {
        return GetChannel().Apply(BIND([=] (const NRpc::IChannelPtr& channel) {
            TTransactionParticipantServiceProxy proxy(channel);
            auto req = builder(&proxy);
            return req->Invoke().template As<void>();
        }));
    }

    void PrepareRequest(const TIntrusivePtr<NRpc::TClientRequest>& request)
    {
        request->SetTimeout(Options_.RpcTimeout);
    }

    TFuture<NRpc::IChannelPtr> GetChannel()
    {
        auto channel = CellDirectory_->FindChannel(CellId_);
        if (channel) {
            return MakeFuture(channel);
        }

        // NB: We rely on background Cell Directory sync.
        return MakeFuture<NRpc::IChannelPtr>(TError(
            NRpc::EErrorCode::Unavailable,
            "No such participant cell %v",
            CellId_));
    }
};

ITransactionParticipantPtr CreateNativeTransactionParticipant(
    TCellDirectoryPtr cellDirectory,
    ITimestampProviderPtr timestampProvider,
    const TCellId& cellId,
    const TTransactionParticipantOptions& options)
{
    return New<TNativeTransactionParticipant>(
        std::move(cellDirectory),
        std::move(timestampProvider),
        cellId,
        options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
