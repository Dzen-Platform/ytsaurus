#ifndef TRANSACTION_DETAIL_INL_H_
#error "Direct inclusion of this file is not allowed, include transaction_detail.h"
// For the sake of sane code completion.
#include "transaction_detail.h"
#endif

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
TTransactionBase<TBase>::TTransactionBase(TTransactionId id)
    : TBase(id)
    , State_(ETransactionState::Active)
{ }

template <class TBase>
ETransactionState TTransactionBase<TBase>::GetPersistentState() const
{
    switch (this->State_) {
        case ETransactionState::TransientCommitPrepared:
        case ETransactionState::TransientAbortPrepared:
            return ETransactionState::Active;
        default:
            return this->State_;
    }
}

template <class TBase>
void TTransactionBase<TBase>::ThrowInvalidState() const
{
    THROW_ERROR_EXCEPTION(
        NTransactionClient::EErrorCode::InvalidTransactionState,
        "Transaction %v is in %Qlv state",
        this->Id_,
        this->State_);
}

template <class TBase>
void TTransactionBase<TBase>::Save(TStreamSaveContext& context) const
{
    using NYT::Save;

    Save(context, Actions_);
}

template <class TBase>
void TTransactionBase<TBase>::Load(TStreamLoadContext& context)
{
    using NYT::Load;

    Load(context, Actions_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
