#include "user.h"
#include "helpers.h"

#include "yt/yt/server/master/security_server/private.h"

#include <yt/yt/server/lib/security_server/proto/security_manager.pb.h>

#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/core/concurrency/config.h>

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NSecurityServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NCrypto;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void TUserRequestLimitsOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("default", &TThis::Default)
        .GreaterThan(0)
        .Default(100);
    registrar.Parameter("per_cell", &TThis::PerCell)
        .Optional();

    registrar.Postprocessor([] (TThis* config) {
        for (const auto& [cellTag, value] : config->PerCell) {
            if (cellTag < NObjectClient::MinValidCellTag || cellTag > NObjectClient::MaxValidCellTag) {
                THROW_ERROR_EXCEPTION("Invalid cell tag %v",
                    cellTag);
            }

            if (value <= 0) {
                THROW_ERROR_EXCEPTION("Invalid limit for cell %v: value %v must be greater than zero",
                    cellTag,
                    value);
            }
        }
    });
}

void TUserRequestLimitsOptions::SetValue(NObjectServer::TCellTag cellTag, std::optional<int> value)
{
    if (cellTag == NObjectClient::InvalidCellTag) {
        Default = value;
    } else {
        YT_VERIFY(value);
        PerCell[cellTag] = *value;
    }
}

std::optional<int> TUserRequestLimitsOptions::GetValue(NObjectServer::TCellTag cellTag) const
{
    if (auto it = PerCell.find(cellTag)) {
        return it->second;
    }
    return Default;
}

////////////////////////////////////////////////////////////////////////////////

void TUserQueueSizeLimitsOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("default", &TThis::Default)
        .GreaterThan(0)
        .Default(100);
    registrar.Parameter("per_cell", &TThis::PerCell)
        .Optional();

    registrar.Postprocessor([] (TThis* config) {
        for (const auto& [cellTag, value] : config->PerCell) {
            if (cellTag < NObjectClient::MinValidCellTag || cellTag > NObjectClient::MaxValidCellTag) {
                THROW_ERROR_EXCEPTION("Invalid cell tag %v",
                    cellTag);
            }

            if (value <= 0) {
                THROW_ERROR_EXCEPTION("Invalid limit for cell %v: value %v must be greater than zero",
                    cellTag,
                    value);
            }
        }
    });
}

void TUserQueueSizeLimitsOptions::SetValue(NObjectServer::TCellTag cellTag, int value)
{
    if (cellTag == NObjectClient::InvalidCellTag) {
        Default = value;
    } else {
        PerCell[cellTag] = value;
    }
}

int TUserQueueSizeLimitsOptions::GetValue(NObjectServer::TCellTag cellTag) const
{
    return PerCell.Value(cellTag, Default);
}

////////////////////////////////////////////////////////////////////////////////

void TUserRequestLimitsConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("read_request_rate", &TThis::ReadRequestRateLimits)
        .DefaultNew();
    registrar.Parameter("write_request_rate", &TThis::WriteRequestRateLimits)
        .DefaultNew();
    registrar.Parameter("request_queue_size", &TThis::RequestQueueSizeLimits)
        .DefaultNew();

    registrar.Postprocessor([] (TThis* config) {
        if (!config->ReadRequestRateLimits) {
            THROW_ERROR_EXCEPTION("\"read_request_rate\" must be set");
        }
        if (!config->WriteRequestRateLimits) {
            THROW_ERROR_EXCEPTION("\"write_request_rate\" must be set");
        }
        if (!config->RequestQueueSizeLimits) {
            THROW_ERROR_EXCEPTION("\"request_queue_size\" must be set");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TSerializableUserRequestLimitsOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("default", &TThis::Default_)
        .GreaterThan(0)
        .Default(100);
    registrar.Parameter("per_cell", &TThis::PerCell_)
        .Optional();
}

TSerializableUserRequestLimitsOptionsPtr TSerializableUserRequestLimitsOptions::CreateFrom(
    const TUserRequestLimitsOptionsPtr& options,
    const IMulticellManagerPtr& multicellManager)
{
    auto result = New<TSerializableUserRequestLimitsOptions>();

    result->Default_ = options->Default;
    result->PerCell_ = CellTagMapToCellNameMap(options->PerCell, multicellManager);

    return result;
}

TUserRequestLimitsOptionsPtr TSerializableUserRequestLimitsOptions::ToLimitsOrThrow(
    const IMulticellManagerPtr& multicellManager) const
{
    auto result = New<TUserRequestLimitsOptions>();
    result->Default = Default_;
    result->PerCell = CellNameMapToCellTagMapOrThrow(PerCell_, multicellManager);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

void TSerializableUserQueueSizeLimitsOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("default", &TThis::Default_)
        .GreaterThan(0)
        .Default(100);
    registrar.Parameter("per_cell", &TThis::PerCell_)
        .Optional();
}

TSerializableUserQueueSizeLimitsOptionsPtr TSerializableUserQueueSizeLimitsOptions::CreateFrom(
    const TUserQueueSizeLimitsOptionsPtr& options,
    const IMulticellManagerPtr& multicellManager)
{
    auto result = New<TSerializableUserQueueSizeLimitsOptions>();

    result->Default_ = options->Default;
    result->PerCell_ = CellTagMapToCellNameMap(options->PerCell, multicellManager);

    return result;
}

TUserQueueSizeLimitsOptionsPtr TSerializableUserQueueSizeLimitsOptions::ToLimitsOrThrow(
    const IMulticellManagerPtr& multicellManager) const
{
    auto result = New<TUserQueueSizeLimitsOptions>();
    result->Default = Default_;
    result->PerCell = CellNameMapToCellTagMapOrThrow(PerCell_, multicellManager);
    return result;
}

////////////////////////////////////////////////////////////////////////////////


void TSerializableUserRequestLimitsConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("read_request_rate", &TThis::ReadRequestRateLimits_)
        .DefaultNew();
    registrar.Parameter("write_request_rate", &TThis::WriteRequestRateLimits_)
        .DefaultNew();
    registrar.Parameter("request_queue_size", &TThis::RequestQueueSizeLimits_)
        .DefaultNew();
}

TSerializableUserRequestLimitsConfigPtr TSerializableUserRequestLimitsConfig::CreateFrom(
    const TUserRequestLimitsConfigPtr& config,
    const IMulticellManagerPtr& multicellManager)
{
    auto result = New<TSerializableUserRequestLimitsConfig>();

    result->ReadRequestRateLimits_ = TSerializableUserRequestLimitsOptions::CreateFrom(config->ReadRequestRateLimits, multicellManager);
    result->WriteRequestRateLimits_ = TSerializableUserRequestLimitsOptions::CreateFrom(config->WriteRequestRateLimits, multicellManager);
    result->RequestQueueSizeLimits_ = TSerializableUserQueueSizeLimitsOptions::CreateFrom(config->RequestQueueSizeLimits, multicellManager);

    return result;
}

TUserRequestLimitsConfigPtr TSerializableUserRequestLimitsConfig::ToConfigOrThrow(
    const IMulticellManagerPtr& multicellManager) const
{
    auto result = New<TUserRequestLimitsConfig>();
    result->ReadRequestRateLimits = ReadRequestRateLimits_->ToLimitsOrThrow(multicellManager);
    result->WriteRequestRateLimits = WriteRequestRateLimits_->ToLimitsOrThrow(multicellManager);
    result->RequestQueueSizeLimits = RequestQueueSizeLimits_->ToLimitsOrThrow(multicellManager);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TUser::TUser(TUserId id)
    : TSubject(id)
    , ObjectServiceRequestLimits_(New<TUserRequestLimitsConfig>())
{ }

TString TUser::GetLowercaseObjectName() const
{
    return Format("user %Qv", Name_);
}

TString TUser::GetCapitalizedObjectName() const
{
    return Format("User %Qv", Name_);
}

void TUser::Save(TSaveContext& context) const
{
    TSubject::Save(context);

    using NYT::Save;
    Save(context, Banned_);
    Save(context, EncryptedPassword_);
    Save(context, PasswordSalt_);
    Save(context, PasswordRevision_);
    Save(context, *ObjectServiceRequestLimits_);
    TNullableIntrusivePtrSerializer<>::Save(context, ChunkServiceUserRequestWeightThrottlerConfig_);
    TNullableIntrusivePtrSerializer<>::Save(context, ChunkServiceUserRequestBytesThrottlerConfig_);
}

void TUser::Load(TLoadContext& context)
{
    TSubject::Load(context);

    using NYT::Load;
    Load(context, Banned_);
    // COMPAT(gritukan)
    if (context.GetVersion() >= EMasterReign::UserPassword) {
        Load(context, EncryptedPassword_);
        Load(context, PasswordSalt_);
        Load(context, PasswordRevision_);
    }
    Load(context, *ObjectServiceRequestLimits_);
    // COMPAT(h0pless)
    if (context.GetVersion() >= EMasterReign::AddPerUserChunkThrottlers) {
        TNullableIntrusivePtrSerializer<>::Load(context, ChunkServiceUserRequestWeightThrottlerConfig_);
        TNullableIntrusivePtrSerializer<>::Load(context, ChunkServiceUserRequestBytesThrottlerConfig_);
    }

    auto profiler = SecurityProfiler
        .WithSparse()
        .WithTag("user", Name_);

    ReadTimeCounter_ = profiler.TimeCounter("/user_read_time");
    WriteTimeCounter_ = profiler.TimeCounter("/user_write_time");
    ReadRequestCounter_ = profiler.Counter("/user_read_request_count");
    WriteRequestCounter_ = profiler.Counter("/user_write_request_count");
    RequestCounter_ = profiler.Counter("/user_request_count");
    RequestQueueSizeSummary_ = profiler.Summary("/user_request_queue_size");
}

int TUser::GetRequestQueueSize() const
{
    return RequestQueueSize_;
}

void TUser::SetRequestQueueSize(int size)
{
    RequestQueueSize_ = size;
    RequestQueueSizeSummary_.Record(size);
}

void TUser::ResetRequestQueueSize()
{
    RequestQueueSize_ = 0;
}

void TUser::SetPassword(std::optional<TString> password)
{
    auto* hydraContext = NHydra::GetCurrentHydraContext();
    PasswordRevision_ = hydraContext->GetVersion().ToRevision();

    if (password) {
        constexpr int SaltLength = 32;
        constexpr int AlphabetSize = 26;

        // NB: This generator is not crypto-safe!
        const auto& rng = hydraContext->RandomGenerator();

        TString salt;
        for (int index = 0; index < SaltLength; ++index) {
            salt += 'A' + rng->Generate<int>() % AlphabetSize;
        }

        EncryptedPassword_ = EncryptPassword(*password, salt);
        PasswordSalt_ = std::move(salt);
    } else {
        EncryptedPassword_ = std::move(password);
        PasswordSalt_ = {};
    }
}

bool TUser::HasPassword() const
{
    return static_cast<bool>(EncryptedPassword_);
}

void TUser::UpdateCounters(const TUserWorkload& workload)
{
    RequestCounter_.Increment(workload.RequestCount);
    switch (workload.Type) {
        case EUserWorkloadType::Read:
            ReadRequestCounter_.Increment(workload.RequestCount);
            ReadTimeCounter_.Add(workload.RequestTime);
            break;
        case EUserWorkloadType::Write:
            WriteRequestCounter_.Increment(workload.RequestCount);
            WriteTimeCounter_.Add(workload.RequestTime);
            break;
        default:
            YT_ABORT();
    }
}

const IReconfigurableThroughputThrottlerPtr& TUser::GetRequestRateThrottler(EUserWorkloadType workloadType)
{
    VERIFY_THREAD_AFFINITY_ANY();

    switch (workloadType) {
        case EUserWorkloadType::Read:
            return ReadRequestRateThrottler_;
        case EUserWorkloadType::Write:
            return WriteRequestRateThrottler_;
        default:
            YT_ABORT();
    }
}

void TUser::SetRequestRateThrottler(
    IReconfigurableThroughputThrottlerPtr throttler,
    EUserWorkloadType workloadType)
{
    switch (workloadType) {
        case EUserWorkloadType::Read:
            ReadRequestRateThrottler_ = std::move(throttler);
            break;
        case EUserWorkloadType::Write:
            WriteRequestRateThrottler_ = std::move(throttler);
            break;
        default:
            YT_ABORT();
    }
}

std::optional<int> TUser::GetRequestRateLimit(EUserWorkloadType type, NObjectServer::TCellTag cellTag) const
{
    switch (type) {
        case EUserWorkloadType::Read:
            return ObjectServiceRequestLimits_->ReadRequestRateLimits->GetValue(cellTag);
        case EUserWorkloadType::Write:
            return ObjectServiceRequestLimits_->WriteRequestRateLimits->GetValue(cellTag);
        default:
            YT_ABORT();
    }
}

void TUser::SetRequestRateLimit(std::optional<int> limit, EUserWorkloadType type, NObjectServer::TCellTag cellTag)
{
    switch (type) {
        case EUserWorkloadType::Read:
            ObjectServiceRequestLimits_->ReadRequestRateLimits->SetValue(cellTag, limit);
            break;
        case EUserWorkloadType::Write:
            ObjectServiceRequestLimits_->WriteRequestRateLimits->SetValue(cellTag, limit);
            break;
        default:
            YT_ABORT();
    }
}

int TUser::GetRequestQueueSizeLimit(NObjectServer::TCellTag cellTag) const
{
    return ObjectServiceRequestLimits_->RequestQueueSizeLimits->GetValue(cellTag);
}

void TUser::SetRequestQueueSizeLimit(int limit, NObjectServer::TCellTag cellTag)
{
    ObjectServiceRequestLimits_->RequestQueueSizeLimits->SetValue(cellTag, limit);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer

