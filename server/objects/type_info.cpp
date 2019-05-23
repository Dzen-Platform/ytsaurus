#include "type_info.h"
#include "private.h"

#include <yp/client/api/proto/autogen.pb.h>

#include <yt/core/misc/cast.h>

#include <util/generic/singleton.h>

namespace NYP::NServer::NObjects {

using namespace google::protobuf;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

TString ReplaceUnderscoresWithSpaces(TString s)
{
    s.Transform([] (int, auto c) {
        if (c == '_') {
            return ' ';
        }
        return c;
    });
    return s;
}

TString Capitalize(TString s)
{
    if (!s.empty()) {
        s.to_upper(0, 1);
    }
    return s;
}

////////////////////////////////////////////////////////////////////////////////

template <class TContainedProtoMessage>
const FileDescriptor* InferProtoFileDescriptor()
{
    auto* messageDescriptor = TContainedProtoMessage::descriptor();
    Y_ASSERT(messageDescriptor);
    return messageDescriptor->file();
}

const FileDescriptor* GetRootProtoFileDescriptor()
{
    // We use TNodeMeta for the inference because it's very likely that
    // TNodeMeta will be in the root file for a long time.
    auto* protoFileDescriptor = InferProtoFileDescriptor<NClient::NApi::NProto::TNodeMeta>();
    Y_ASSERT(protoFileDescriptor);
    YCHECK(protoFileDescriptor->name().EndsWith("client/api/proto/autogen.proto"));
    return protoFileDescriptor;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

struct TTypeInfo
{
    TTypeInfo() = default;

    TTypeInfo(TString capitalizedHumanReadableName, TString humanReadableName)
        : CapitalizedHumanReadableName(std::move(capitalizedHumanReadableName))
        , HumanReadableName(std::move(humanReadableName))
    { }

    TString CapitalizedHumanReadableName;
    TString HumanReadableName;
};

////////////////////////////////////////////////////////////////////////////////

class TTypeRegistry
{
public:
    TTypeRegistry()
    {
        try {
            Initialize();
        } catch (...) {
            Y_UNREACHABLE();
        }
    }

    const TTypeInfo* FindInfo(EObjectType type) const
    {
        return type >= TEnumTraits<EObjectType>::GetMinValue() && type <= TEnumTraits<EObjectType>::GetMaxValue()
            ? TypeInfos_[type].get()
            : nullptr;
    }

    static TTypeRegistry* Get()
    {
        return Singleton<TTypeRegistry>();
    }

private:
    TEnumIndexedVector<std::unique_ptr<TTypeInfo>, EObjectType> TypeInfos_;

    void Initialize()
    {
        InitializeInternalTypesInfo();
        InitializeExternalTypesInfo();
    }

    void AddTypeInfo(EObjectType type, std::unique_ptr<TTypeInfo> typeInfo)
    {
        YCHECK(typeInfo);
        YCHECK(!TypeInfos_[type]);
        TypeInfos_[type] = std::move(typeInfo);
        YT_LOG_DEBUG("Initialized type info (Type: %v, HumanReadableName: %v, CapitalizedHumanReadableName: %v)",
            type,
            TypeInfos_[type]->HumanReadableName,
            TypeInfos_[type]->CapitalizedHumanReadableName);
    }

    void InitializeInternalTypesInfo()
    {
        AddTypeInfo(
            EObjectType::NetworkModule,
            std::make_unique<TTypeInfo>(
                "network module",
                "Network module"));
    }

    void InitializeExternalTypesInfo()
    {
        THashSet<TString> visitedProtoFileNames;
        InitializeTypesFromProtoFilesRecursively(
            GetRootProtoFileDescriptor(),
            &visitedProtoFileNames);
    }

    void InitializeTypesFromProtoFilesRecursively(
        const FileDescriptor* rootProtoFileDescriptor,
        THashSet<TString>* visitedProtoFileNames)
    {
        Y_ASSERT(rootProtoFileDescriptor);
        if (!visitedProtoFileNames->insert(rootProtoFileDescriptor->name()).second) {
            return;
        }
        InitializeTypesFromProtoFile(rootProtoFileDescriptor);
        for (int dependencyIndex = 0;
            dependencyIndex < rootProtoFileDescriptor->dependency_count();
            ++dependencyIndex)
        {
            InitializeTypesFromProtoFilesRecursively(
                rootProtoFileDescriptor->dependency(dependencyIndex),
                visitedProtoFileNames);
        }
    }

    void InitializeTypesFromProtoFile(const FileDescriptor* protoFileDescriptor)
    {
        Y_ASSERT(protoFileDescriptor);
        YT_LOG_DEBUG("Initializing types info from file (FileName: %v)",
            protoFileDescriptor->name());
        auto protoTypeInfos = protoFileDescriptor->options().GetRepeatedExtension(NClient::NApi::NProto::object_type);
        for (const auto& protoTypeInfo : protoTypeInfos) {
            auto type = EObjectType::Null;
            YCHECK(TryEnumCast(protoTypeInfo.type_value(), &type));

            auto typeInfo = std::make_unique<TTypeInfo>();
            if (protoTypeInfo.has_human_readable_name()) {
                typeInfo->HumanReadableName = protoTypeInfo.human_readable_name();
            } else {
                typeInfo->HumanReadableName = ReplaceUnderscoresWithSpaces(protoTypeInfo.snake_case_name());
            }
            typeInfo->CapitalizedHumanReadableName = Capitalize(typeInfo->HumanReadableName);

            AddTypeInfo(type, std::move(typeInfo));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TStringBuf GetCapitalizedHumanReadableTypeName(EObjectType type)
{
    auto* info = TTypeRegistry::Get()->FindInfo(type);
    YCHECK(info);
    return info->CapitalizedHumanReadableName;
}

TStringBuf GetHumanReadableTypeName(EObjectType type)
{
    auto* info = TTypeRegistry::Get()->FindInfo(type);
    YCHECK(info);
    return info->HumanReadableName;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
