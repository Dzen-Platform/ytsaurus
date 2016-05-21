#include "functions_cache.h"
#include "functions_cg.h"
#include "private.h"

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/config.h>
#include <yt/ytlib/api/file_reader.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/read_limit.h>

#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/object_ypath_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/file_client/file_chunk_reader.h>

#include <yt/core/logging/log.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/guid.h>
#include <yt/core/misc/async_cache.h>
#include <yt/core/misc/expiring_cache.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NQueryClient {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYPath;
using namespace NFileClient;

using NObjectClient::TObjectServiceProxy;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = QueryClientLogger;

////////////////////////////////////////////////////////////////////////////////

class TCypressFunctionDescriptor
    : public NYTree::TYsonSerializable
{
public:
    Stroka Name;
    std::vector<TDescriptorType> ArgumentTypes;
    TNullable<TDescriptorType> RepeatedArgumentType;
    TDescriptorType ResultType;
    ECallingConvention CallingConvention;

    TCypressFunctionDescriptor()
    {
        RegisterParameter("name", Name)
            .NonEmpty();
        RegisterParameter("argument_types", ArgumentTypes);
        RegisterParameter("result_type", ResultType);
        RegisterParameter("calling_convention", CallingConvention);
        RegisterParameter("repeated_argument_type", RepeatedArgumentType)
            .Default();
    }

    std::vector<TType> GetArgumentsTypes()
    {
        std::vector<TType> argumentTypes;
        for (const auto& type: ArgumentTypes) {
            argumentTypes.push_back(type.Type);
        }
        return argumentTypes;
    }
};

DECLARE_REFCOUNTED_CLASS(TCypressFunctionDescriptor)
DEFINE_REFCOUNTED_TYPE(TCypressFunctionDescriptor)

class TCypressAggregateDescriptor
    : public NYTree::TYsonSerializable
{
public:
    Stroka Name;
    TDescriptorType ArgumentType;
    TDescriptorType StateType;
    TDescriptorType ResultType;
    ECallingConvention CallingConvention;

    TCypressAggregateDescriptor()
    {
        RegisterParameter("name", Name)
            .NonEmpty();
        RegisterParameter("argument_type", ArgumentType);
        RegisterParameter("state_type", StateType);
        RegisterParameter("result_type", ResultType);
        RegisterParameter("calling_convention", CallingConvention);
    }
};

DECLARE_REFCOUNTED_CLASS(TCypressAggregateDescriptor)
DEFINE_REFCOUNTED_TYPE(TCypressAggregateDescriptor)

DEFINE_REFCOUNTED_TYPE(TExternalCGInfo)

////////////////////////////////////////////////////////////////////////////////

const Stroka FunctionDescriptorAttribute = "function_descriptor";
const Stroka AggregateDescriptorAttribute = "aggregate_descriptor";

////////////////////////////////////////////////////////////////////////////////

TExternalCGInfo::TExternalCGInfo()
    : NodeDirectory(New<NNodeTrackerClient::TNodeDirectory>())
{ }

Stroka GetUdfDescriptorPath(const TYPath& registryPath, const Stroka& functionName)
{
    return registryPath + "/" + ToYPathLiteral(to_lower(functionName));
}

std::vector<TExternalFunctionSpec> LookupAllUdfDescriptors(
    const std::vector<Stroka>& functionNames,
    const Stroka& udfRegistryPath,
    NApi::IClientPtr client)
{
    using NObjectClient::TObjectYPathProxy;
    using NApi::EMasterChannelKind;
    using NObjectClient::FromObjectId;
    using NNodeTrackerClient::TNodeDirectory;
    using NChunkClient::TReadRange;

    std::vector<TExternalFunctionSpec> result;

    LOG_DEBUG("Looking for UDFs in Cypress");

    auto attributeFilter = std::vector<Stroka>{
        FunctionDescriptorAttribute,
        AggregateDescriptorAttribute};

    TObjectServiceProxy proxy(client->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower));
    auto batchReq = proxy.ExecuteBatch();

    for (const auto& functionName : functionNames) {
        auto path = GetUdfDescriptorPath(udfRegistryPath, functionName);

        auto getReq = TYPathProxy::Get(path);

        NYT::ToProto(getReq->mutable_attributes()->mutable_keys(), attributeFilter);
        batchReq->AddRequest(getReq, "get_attributes");

        auto basicAttributesReq = TObjectYPathProxy::GetBasicAttributes(path);
        batchReq->AddRequest(basicAttributesReq, "get_basic_attributes");
    }

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();

    auto getRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_attributes");
    auto basicAttributesRspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_basic_attributes");

    yhash<NObjectClient::TCellTag, std::vector<size_t>> infoByCellTags;

    for (int index = 0; index < functionNames.size(); ++index) {
        const auto& functionName = functionNames[index];
        auto path = GetUdfDescriptorPath(udfRegistryPath, functionName);

        auto getRspOrError = getRspsOrError[index];

        THROW_ERROR_EXCEPTION_IF_FAILED(getRspOrError, "Failed to find implementation of function %Qv in Cypress",
            functionName);

        auto getRsp = getRspOrError
            .ValueOrThrow();

        auto basicAttrsRsp = basicAttributesRspsOrError[index]
            .ValueOrThrow();

        auto item = ConvertToNode(NYson::TYsonString(getRsp->value()));
        auto objectId = NYT::FromProto<NObjectClient::TObjectId>(basicAttrsRsp->object_id());
        auto cellTag = basicAttrsRsp->cell_tag();

        LOG_DEBUG("Found implementation of function %Qv in Cypress (Descriptor: %v)",
            functionName,
            ConvertToYsonString(item, NYson::EYsonFormat::Text).Data());

        TExternalFunctionSpec cypressInfo;
        cypressInfo.Descriptor = item;
        cypressInfo.FilePath = path;
        cypressInfo.ObjectId = objectId;
        cypressInfo.CellTag = cellTag;

        result.push_back(cypressInfo);

        infoByCellTags[cellTag].push_back(index);
    }

    for (const auto& infoByCellTag : infoByCellTags) {
        const auto& cellTag = infoByCellTag.first;

        TObjectServiceProxy proxy(client->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower, cellTag));
        auto fetchBatchReq = proxy.ExecuteBatch();

        for (auto resultIndex : infoByCellTag.second) {
            auto fetchReq = TFileYPathProxy::Fetch(FromObjectId(result[resultIndex].ObjectId));
            fetchReq->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
            NYT::ToProto(fetchReq->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
            fetchBatchReq->AddRequest(fetchReq);
        }

        auto fetchBatchRsp = WaitFor(fetchBatchReq->Invoke())
            .ValueOrThrow();

        for (size_t rspIndex = 0; rspIndex < infoByCellTag.second.size(); ++rspIndex) {
            auto resultIndex = infoByCellTag.second[rspIndex];

            auto fetchRsp = fetchBatchRsp->GetResponse<TFileYPathProxy::TRspFetch>(rspIndex)
                .ValueOrThrow();

            auto nodeDirectory = New<TNodeDirectory>();

            NChunkClient::ProcessFetchResponse(
                client,
                fetchRsp,
                cellTag,
                nodeDirectory,
                10000,
                Logger,
                &result[resultIndex].Chunks);

            YCHECK(!result[resultIndex].Chunks.empty());

            nodeDirectory->DumpTo(&result[resultIndex].NodeDirectory);
        }
    }

    return result;
}

void AppendUdfDescriptors(
    const TTypeInferrerMapPtr& typers,
    const TExternalCGInfoPtr& cgInfo,
    const std::vector<Stroka>& names,
    const std::vector<TExternalFunctionSpec>& external)
{
    YCHECK(names.size() == external.size());

    LOG_DEBUG("Appending UDF %v descriptors", external.size());

    for (size_t index = 0; index < external.size(); ++index) {
        const auto& item = external[index];
        const auto& descriptor = item.Descriptor;

        const auto& name = names[index];

        LOG_DEBUG("Appending UDF descriptor %v = %v",
            name,
            ConvertToYsonString(descriptor, NYson::EYsonFormat::Text).Data());

        if (!descriptor) {
            continue;
        }
        cgInfo->NodeDirectory->MergeFrom(item.NodeDirectory);

        const auto& attributes = descriptor->Attributes();

        auto functionDescriptor = attributes.Find<TCypressFunctionDescriptorPtr>(
            FunctionDescriptorAttribute);
        auto aggregateDescriptor = attributes.Find<TCypressAggregateDescriptorPtr>(
            AggregateDescriptorAttribute);

        if (bool(functionDescriptor) == bool(aggregateDescriptor)) {
            THROW_ERROR_EXCEPTION(
                "Item must have either function descriptor or aggregate descriptor");
        }

        const auto& chunks = item.Chunks;

        // NB(lukyan): Aggregate initialization is not supported in GCC in this case
        TExternalFunctionImpl functionBody;
        functionBody.Name = name;
        functionBody.ChunkSpecs = chunks;

        LOG_DEBUG("Appending UDF descriptor %v", Format("{%v}", JoinToString(chunks, [] (
            TStringBuilder* builder,
            const NChunkClient::NProto::TChunkSpec& chunkSpec)
        {
            builder->AppendFormat("%v", NYT::FromProto<TGuid>(chunkSpec.chunk_id()));
        })));

        if (functionDescriptor) {
            LOG_DEBUG("Appending function UDF descriptor %v", name);

            functionBody.IsAggregate = false;
            functionBody.SymbolName = functionDescriptor->Name;
            functionBody.CallingConvention = functionDescriptor->CallingConvention;
            functionBody.RepeatedArgType = functionDescriptor->RepeatedArgumentType
                ? functionDescriptor->RepeatedArgumentType->Type
                : EValueType::Null,
            functionBody.RepeatedArgIndex = int(functionDescriptor->GetArgumentsTypes().size());

            auto typer = functionDescriptor->RepeatedArgumentType
                ? New<TFunctionTypeInferrer>(
                    std::unordered_map<TTypeArgument, TUnionType>(),
                    functionDescriptor->GetArgumentsTypes(),
                    functionDescriptor->RepeatedArgumentType->Type,
                    functionDescriptor->ResultType.Type)
                : New<TFunctionTypeInferrer>(
                    std::unordered_map<TTypeArgument, TUnionType>(),
                    functionDescriptor->GetArgumentsTypes(),
                    functionDescriptor->ResultType.Type);

            typers->emplace(name, typer);
            cgInfo->push_back(std::move(functionBody));
        }

        if (aggregateDescriptor) {
            LOG_DEBUG("Appending aggregate UDF descriptor %v", name);

            functionBody.IsAggregate = true;
            functionBody.SymbolName = aggregateDescriptor->Name;
            functionBody.CallingConvention = aggregateDescriptor->CallingConvention;
            functionBody.RepeatedArgType = EValueType::Null;
            functionBody.RepeatedArgIndex = -1;

            auto typer = New<TAggregateTypeInferrer>(
                std::unordered_map<TTypeArgument, TUnionType>(),
                aggregateDescriptor->ArgumentType.Type,
                aggregateDescriptor->ResultType.Type,
                aggregateDescriptor->StateType.Type);

            typers->emplace(name, typer);
            cgInfo->push_back(std::move(functionBody));
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_REFCOUNTED_TYPE(IFunctionRegistry)

class TCypressFunctionRegistry
    : public TExpiringCache<Stroka, TExternalFunctionSpec>
    , public IFunctionRegistry
{
public:
    typedef TExpiringCache<Stroka, TExternalFunctionSpec> TBase;

    TCypressFunctionRegistry(
        const Stroka& registryPath,
        TExpiringCacheConfigPtr config,
        TWeakPtr<NApi::IClient> client,
        IInvokerPtr invoker)
        : TBase(config)
        , RegistryPath_(registryPath)
        , Client_(client)
        , Invoker_(invoker)
    { }

    virtual TFuture<TExternalFunctionSpec> DoGet(const Stroka& key)
    {
        return DoGetMany({key})
            .Apply(BIND([] (const std::vector<TExternalFunctionSpec>& result) {
                return result[0];
            }));
    }

    virtual TFuture<std::vector<TExternalFunctionSpec>> DoGetMany(const std::vector<Stroka>& keys)
    {
        return BIND(LookupAllUdfDescriptors, keys, RegistryPath_, Client_.Lock())
            .AsyncVia(Invoker_)
            .Run();
    }

    virtual TFuture<std::vector<TExternalFunctionSpec>> FetchFunctions(const std::vector<Stroka>& names) override
    {
        return Get(names);
    }

private:
    Stroka RegistryPath_;
    TWeakPtr<NApi::IClient> Client_;
    IInvokerPtr Invoker_;

};

////////////////////////////////////////////////////////////////////////////////

IFunctionRegistryPtr CreateFunctionRegistryCache(
    const Stroka& registryPath,
    TExpiringCacheConfigPtr config,
    TWeakPtr<NApi::IClient> client,
    IInvokerPtr invoker)
{
    return New<TCypressFunctionRegistry>(registryPath, std::move(config), std::move(client), std::move(invoker));
}

////////////////////////////////////////////////////////////////////////////////

struct TFunctionImplKey
{
    std::vector<NChunkClient::NProto::TChunkSpec> ChunkSpecs;

    // Hasher.
    operator size_t() const;

    // Comparer.
    bool operator == (const TFunctionImplKey& other) const;
};

TFunctionImplKey::operator size_t() const
{
    size_t result = 0;

    for (const auto& spec : ChunkSpecs) {
        auto id = NYT::FromProto<TGuid>(spec.chunk_id());
        result = HashCombine(result, id);
    }

    return result;
}

bool TFunctionImplKey::operator == (const TFunctionImplKey& other) const
{
    if (ChunkSpecs.size() != other.ChunkSpecs.size())
        return false;

    for (int index = 0; index < ChunkSpecs.size(); ++index) {
        const auto& lhs = ChunkSpecs[index];
        const auto& rhs = other.ChunkSpecs[index];

        auto leftId = NYT::FromProto<TGuid>(lhs.chunk_id());
        auto rightId = NYT::FromProto<TGuid>(rhs.chunk_id());

        if (leftId != rightId)
            return false;
    }

    return true;
}

Stroka ToString(const TFunctionImplKey& key)
{
    return Format("{%v}", JoinToString(key.ChunkSpecs, [] (
        TStringBuilder* builder,
        const NChunkClient::NProto::TChunkSpec& chunkSpec)
    {
        builder->AppendFormat("%v", NYT::FromProto<TGuid>(chunkSpec.chunk_id()));
    }));
}

class TFunctionImplCacheEntry
    : public TAsyncCacheValueBase<TFunctionImplKey, TFunctionImplCacheEntry>
{
public:
    TFunctionImplCacheEntry(const TFunctionImplKey& key, TSharedRef file)
        : TAsyncCacheValueBase(key)
        , File(std::move(file))
    { }

    TSharedRef File;
};

typedef TIntrusivePtr<TFunctionImplCacheEntry> TFunctionImplCacheEntryPtr;

class TFunctionImplCache
    : public TAsyncSlruCacheBase<TFunctionImplKey, TFunctionImplCacheEntry>
{
public:
    TFunctionImplCache(
        const TSlruCacheConfigPtr& config,
        TWeakPtr<NApi::IClient> client)
        : TAsyncSlruCacheBase(config)
        , Client_(client)
    { }

    TSharedRef DoFetch(const TFunctionImplKey& key, TNodeDirectoryPtr nodeDirectory)
    {
        auto client = Client_.Lock();
        YCHECK(client);
        auto chunks = key.ChunkSpecs;

        auto reader = NFileClient::CreateFileMultiChunkReader(
            New<NApi::TFileReaderConfig>(),
            New<NChunkClient::TMultiChunkReaderOptions>(),
            client,
            NNodeTrackerClient::TNodeDescriptor(),
            client->GetConnection()->GetBlockCache(),
            std::move(nodeDirectory),
            std::move(chunks));

        LOG_DEBUG("Downloading implementation for UDF function (Chunks: %v)", key);

        std::vector<TSharedRef> blocks;
        while (true) {
            TSharedRef block;
            if (!reader->ReadBlock(&block)) {
                break;
            }

            if (block) {
                blocks.push_back(std::move(block));
            }

            WaitFor(reader->GetReadyEvent())
                .ThrowOnError();
        }

        i64 size = GetByteSize(blocks);
        YCHECK(size);
        auto file = TSharedMutableRef::Allocate(size);
        auto memoryOutput = TMemoryOutput(file.Begin(), size);

        for (const auto& block : blocks) {
            memoryOutput.Write(block.Begin(), block.Size());
        }

        return file;
    }

    TFuture<TFunctionImplCacheEntryPtr> FetchImplementation(
        const TFunctionImplKey& key,
        TNodeDirectoryPtr nodeDirectory)
    {
        auto cookie = BeginInsert(key);
        if (cookie.IsActive()) {
            try {
                auto file = DoFetch(key, std::move(nodeDirectory));
                cookie.EndInsert(New<TFunctionImplCacheEntry>(key, file));
            } catch (const std::exception& ex) {
                cookie.Cancel(TError(ex).Wrap("Failed to download function implementation"));
            }
        }
        return cookie.GetValue();
    }

private:
    TWeakPtr<NApi::IClient> Client_;

};

DEFINE_REFCOUNTED_TYPE(TFunctionImplCache)

TFunctionImplCachePtr CreateFunctionImplCache(
    const TSlruCacheConfigPtr& config,
    TWeakPtr<NApi::IClient> client)
{
    return New<TFunctionImplCache>(config, client);
}

////////////////////////////////////////////////////////////////////////////////

TSharedRef GetImplFingerprint(const std::vector<NChunkClient::NProto::TChunkSpec>& chunks)
{
    auto size = chunks.size();
    auto fingerprint = TSharedMutableRef::Allocate(2 * sizeof(ui64) * size);
    auto memoryOutput = TMemoryOutput(fingerprint.Begin(), fingerprint.Size());

    for (const auto& chunk : chunks) {
        auto id = NYT::FromProto<TGuid>(chunk.chunk_id());
        memoryOutput.Write(id.Parts64, 2 * sizeof(ui64));
    }

    return fingerprint;
}

void DoFetchImplementations(
    const TFunctionProfilerMapPtr& functionProfilers,
    const TAggregateProfilerMapPtr& aggregateProfilers,
    const TConstExternalCGInfoPtr& externalCGInfo,
    std::function<TSharedRef(const TExternalFunctionImpl&)> doFetch)
{
    for (const auto& info : *externalCGInfo) {
        const auto& name = info.Name;

        LOG_DEBUG("Fetching implementation for UDF function %v", name);

        auto impl = doFetch(info);
        YCHECK(!impl.Empty());

        if (info.IsAggregate) {
            aggregateProfilers->emplace(name, New<TExternalAggregateCodegen>(
                name, impl, info.CallingConvention, GetImplFingerprint(info.ChunkSpecs)));
        } else {
            functionProfilers->emplace(name, New<TExternalFunctionCodegen>(
                name,
                info.SymbolName,
                impl,
                info.CallingConvention,
                info.RepeatedArgType,
                info.RepeatedArgIndex,
                GetImplFingerprint(info.ChunkSpecs)));
        }
    }
}

void FetchImplementations(
    const TFunctionProfilerMapPtr& functionProfilers,
    const TAggregateProfilerMapPtr& aggregateProfilers,
    const TConstExternalCGInfoPtr& externalCGInfo,
    TFunctionImplCachePtr cache)
{
    DoFetchImplementations(
        functionProfilers,
        aggregateProfilers,
        externalCGInfo,
        [&] (const TExternalFunctionImpl& info) {
            TFunctionImplKey key;
            key.ChunkSpecs = info.ChunkSpecs;
            return WaitFor(cache->FetchImplementation(key, externalCGInfo->NodeDirectory))
                .ValueOrThrow()->File;
        });
}

void FetchJobImplementations(
    const TFunctionProfilerMapPtr& functionProfilers,
    const TAggregateProfilerMapPtr& aggregateProfilers,
    const TConstExternalCGInfoPtr& externalCGInfo,
    Stroka implementationPath)
{
    DoFetchImplementations(
        functionProfilers,
        aggregateProfilers,
        externalCGInfo,
        [&] (const TExternalFunctionImpl& info) {
            auto path = implementationPath + "/" + info.Name;
            TFileInput file(path);
            return TSharedRef::FromString(file.ReadAll());
        });
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TDescriptorType& value, NYson::IYsonConsumer* consumer)
{
    using namespace NYTree;

    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("tag").Value(ETypeCategory(value.Type.Tag()))
            .DoIf(value.Type.TryAs<TTypeArgument>(), [&] (TFluentMap fluent) {
                fluent.Item("value").Value(value.Type.As<TTypeArgument>());
            })
            .DoIf(value.Type.TryAs<TUnionType>(), [&] (TFluentMap fluent) {
                fluent.Item("value").Value(value.Type.As<TUnionType>());
            })
            .DoIf(value.Type.TryAs<EValueType>(), [&] (TFluentMap fluent) {
                fluent.Item("value").Value(value.Type.As<EValueType>());
            })
        .EndMap();
}

void Deserialize(TDescriptorType& value, NYTree::INodePtr node)
{
    using namespace NYTree;

    auto mapNode = node->AsMap();

    auto tagNode = mapNode->GetChild("tag");
    ETypeCategory tag;
    Deserialize(tag, tagNode);

    auto valueNode = mapNode->GetChild("value");
    switch (tag) {
        case ETypeCategory::TypeArgument:
            {
                TTypeArgument type;
                Deserialize(type, valueNode);
                value.Type = type;
                break;
            }
        case ETypeCategory::UnionType:
            {
                TUnionType type;
                Deserialize(type, valueNode);
                value.Type = type;
                break;
            }
        case ETypeCategory::ConcreteType:
            {
                EValueType type;
                Deserialize(type, valueNode);
                value.Type = type;
                break;
            }
        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TExternalFunctionImpl* proto, const TExternalFunctionImpl& object)
{
    proto->set_is_aggregate(object.IsAggregate);
    proto->set_name(object.Name);
    proto->set_symbol_name(object.SymbolName);
    proto->set_calling_convention(int(object.CallingConvention));
    NYT::ToProto(proto->mutable_chunk_specs(), object.ChunkSpecs);

    TDescriptorType descriptorType;
    descriptorType.Type = object.RepeatedArgType;

    proto->set_repeated_arg_type(ConvertToYsonString(descriptorType).Data());
    proto->set_repeated_arg_index(object.RepeatedArgIndex);
}

TExternalFunctionImpl FromProto(const NProto::TExternalFunctionImpl& serialized)
{
    TExternalFunctionImpl result;
    result.IsAggregate = serialized.is_aggregate();
    result.Name = serialized.name();
    result.SymbolName = serialized.symbol_name();
    result.CallingConvention = ECallingConvention(serialized.calling_convention());
    result.ChunkSpecs = NYT::FromProto<std::vector<NChunkClient::NProto::TChunkSpec>>(serialized.chunk_specs());

    result.RepeatedArgType = ConvertTo<TDescriptorType>(NYson::TYsonString(serialized.repeated_arg_type())).Type;
    result.RepeatedArgIndex = serialized.repeated_arg_index();

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
