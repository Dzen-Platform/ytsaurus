#pragma once

#include "public.h"

#include <yt/core/misc/ref.h>

#include <yt/core/rpc/proto/rpc.pb.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM_WITH_UNDERLYING_TYPE(EMessageType, ui32,
    ((Unknown)              (         0))
    ((Request)              (0x69637072)) // rpci
    ((RequestCancelation)   (0x63637072)) // rpcc
    ((Response)             (0x6f637072)) // rpco
);

////////////////////////////////////////////////////////////////////////////////

TSharedRefArray CreateRequestMessage(
    const NProto::TRequestHeader& header,
    const TSharedRef& body,
    const std::vector<TSharedRef>& attachments);

TSharedRefArray CreateRequestMessage(
    const NProto::TRequestHeader& header,
    const TSharedRefArray& data);

TSharedRefArray CreateRequestCancelationMessage(
    const NProto::TRequestCancelationHeader& header);

TSharedRefArray CreateResponseMessage(
    const NProto::TResponseHeader& header,
    const TSharedRef& body,
    const std::vector<TSharedRef>& attachments);

TSharedRefArray CreateResponseMessage(
    const ::google::protobuf::MessageLite& body,
    const std::vector<TSharedRef>& attachments = std::vector<TSharedRef>());

TSharedRefArray CreateErrorResponseMessage(
    const NProto::TResponseHeader& header);

TSharedRefArray CreateErrorResponseMessage(
    TRequestId requestId,
    const TError& error);

TSharedRefArray CreateErrorResponseMessage(
    const TError& error);

////////////////////////////////////////////////////////////////////////////////

EMessageType GetMessageType(const TSharedRefArray& message);

bool ParseRequestHeader(
    const TSharedRefArray& message,
    NProto::TRequestHeader* header);

TSharedRefArray SetRequestHeader(
    const TSharedRefArray& message,
    const NProto::TRequestHeader& header);

bool ParseResponseHeader(
    const TSharedRefArray& message,
    NProto::TResponseHeader* header);

TSharedRefArray SetResponseHeader(
    const TSharedRefArray& message,
    const NProto::TResponseHeader& header);

void MergeRequestHeaderExtensions(
    NProto::TRequestHeader* to,
    const NProto::TRequestHeader& from);

bool ParseRequestCancelationHeader(
    const TSharedRefArray& message,
    NProto::TRequestCancelationHeader* header);

i64 GetMessageBodySize(const TSharedRefArray& message);
int GetMessageAttachmentCount(const TSharedRefArray& message);
i64 GetTotalMessageAttachmentSize(const TSharedRefArray& message);

TError CheckBusMessageLimits(const TSharedRefArray& message);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
