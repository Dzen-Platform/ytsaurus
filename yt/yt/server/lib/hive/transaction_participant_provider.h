#pragma once

#include "public.h"

#include <yt/yt/ytlib/hive/public.h>

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/object_client/public.h>

#include <yt/yt/ytlib/transaction_client/public.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

struct ITransactionParticipantProvider
    : public virtual TRefCounted
{
    virtual NHiveClient::ITransactionParticipantPtr TryCreate(
        TCellId cellId,
        const NApi::TTransactionParticipantOptions& options) = 0;
};

DEFINE_REFCOUNTED_TYPE(ITransactionParticipantProvider)

////////////////////////////////////////////////////////////////////////////////

ITransactionParticipantProviderPtr CreateTransactionParticipantProvider(
    NHiveClient::TCellDirectoryPtr cellDirectory,
    NTransactionClient::ITimestampProviderPtr timestampProvider,
    const NObjectClient::TCellTagList& cellTags);

ITransactionParticipantProviderPtr CreateTransactionParticipantProvider(
    NApi::NNative::IConnectionPtr connection);

ITransactionParticipantProviderPtr CreateTransactionParticipantProvider(
    NHiveClient::TClusterDirectoryPtr clusterDirectory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
