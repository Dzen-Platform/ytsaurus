#pragma once

#include "private.h"

#include <yt/server/lib/misc/config.h>

#include <yt/ytlib/api/native/config.h>

#include <yt/client/ypath/rich.h>

#include <yt/core/concurrency/config.h>
#include <yt/core/ytree/fluent.h>

namespace NYT::NClickHouseServer {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TUserConfig
    : public TYsonSerializable
{
public:
    // This field is overriden by DefaultProfile in TEngineConfig.
    THashMap<TString, THashMap<TString, INodePtr>> Profiles;
    IMapNodePtr Quotas;
    IMapNodePtr UserTemplate;
    IMapNodePtr Users;

    TUserConfig()
    {
        RegisterParameter("profiles", Profiles)
            .Default();

        RegisterParameter("quotas", Quotas)
            .Default(BuildYsonNodeFluently()
                .BeginMap()
                    .Item("default").BeginMap()
                        .Item("interval").BeginMap()
                            .Item("duration").Value(3600)
                            .Item("errors").Value(0)
                            .Item("execution_time").Value(0)
                            .Item("queries").Value(0)
                            .Item("read_rows").Value(0)
                            .Item("result_rows").Value(0)
                        .EndMap()
                    .EndMap()
                .EndMap()->AsMap());

        RegisterParameter("user_template", UserTemplate)
            .Default(BuildYsonNodeFluently()
                .BeginMap()
                    .Item("networks").BeginMap()
                        .Item("ip").Value("::/0")
                    .EndMap()
                    .Item("password").Value("")
                    .Item("profile").Value("default")
                    .Item("quota").Value("default")
                .EndMap()->AsMap());

        RegisterParameter("users", Users)
            .Default(BuildYsonNodeFluently().BeginMap().EndMap()->AsMap());
    }
};

DEFINE_REFCOUNTED_TYPE(TUserConfig);

////////////////////////////////////////////////////////////////////////////////

class TDictionarySourceYtConfig
    : public TYsonSerializable
{
public:
    TDictionarySourceYtConfig()
    {
        RegisterParameter("path", Path);
    }

    NYPath::TRichYPath Path;
};

DEFINE_REFCOUNTED_TYPE(TDictionarySourceYtConfig);

////////////////////////////////////////////////////////////////////////////////

//! Source configuration.
//! Extra supported configuration type is "yt".
//! See: https://clickhouse.yandex/docs/en/query_language/dicts/external_dicts_dict_sources/
class TDictionarySourceConfig
    : public TYsonSerializable
{
public:
    TDictionarySourceConfig()
    {
        RegisterParameter("yt", Yt)
            .Default(nullptr);
    }

    // TODO(max42): proper value omission.
    TDictionarySourceYtConfigPtr Yt;
};

DEFINE_REFCOUNTED_TYPE(TDictionarySourceConfig);

////////////////////////////////////////////////////////////////////////////////

//! External dictionary configuration.
//! See: https://clickhouse.yandex/docs/en/query_language/dicts/external_dicts_dict/
class TDictionaryConfig
    : public TYsonSerializable
{
public:
    TDictionaryConfig()
    {
        RegisterParameter("name", Name);
        RegisterParameter("source", Source);
        RegisterParameter("layout", Layout);
        RegisterParameter("structure", Structure);
        RegisterParameter("lifetime", Lifetime);
    }

    TString Name;

    //! Source configuration.
    TDictionarySourceConfigPtr Source;

    //! Layout configuration.
    //! See: https://clickhouse.yandex/docs/en/query_language/dicts/external_dicts_dict_layout/
    IMapNodePtr Layout;

    //! Structure configuration.
    //! See: https://clickhouse.yandex/docs/en/query_language/dicts/external_dicts_dict_structure/
    IMapNodePtr Structure;

    //! Lifetime configuration.
    //! See: https://clickhouse.yandex/docs/en/query_language/dicts/external_dicts_dict_lifetime/
    INodePtr Lifetime;
};

DEFINE_REFCOUNTED_TYPE(TDictionaryConfig)

////////////////////////////////////////////////////////////////////////////////

class TEngineConfig
    : public TYsonSerializable
{
public:
    TEngineConfig()
    {
        RegisterParameter("users", Users)
            .DefaultNew();

        RegisterParameter("data_path", DataPath)
            .Default("data");

        RegisterParameter("log_level", LogLevel)
            .Default("trace");

        RegisterParameter("cypress_root_path", CypressRootPath)
            .Default("//sys/clickhouse");

        RegisterParameter("listen_hosts", ListenHosts)
            .Default(std::vector<TString> {"::"});

        RegisterParameter("settings", Settings)
            .Optional()
            .MergeBy(EMergeStrategy::Combine);

        RegisterParameter("dictionaries", Dictionaries)
            .Default();

        RegisterParameter("path_to_regions_hierarchy_file", PathToRegionsHierarchyFile)
            .Default("./geodata/regions_hierarchy.txt");

        RegisterParameter("path_to_regions_name_files", PathToRegionsNameFiles)
            .Default("./geodata/");

        RegisterPreprocessor([&] {
            Settings["readonly"] = ConvertToNode(2);
            Settings["max_memory_usage_for_all_queries"] = ConvertToNode(9_GB);
            Settings["max_threads"] = ConvertToNode(32);
            Settings["max_concurrent_queries_for_user"] = ConvertToNode(10);
        });

        RegisterPostprocessor([&] {
            auto& userDefaultProfile = Users->Profiles["default"];
            for (auto& [key, value] : Settings) {
                userDefaultProfile[key] = value;
            }

            Settings = userDefaultProfile;
        });

        SetUnrecognizedStrategy(EUnrecognizedStrategy::KeepRecursive);
    }

    //! A map setting CH security policy.
    TUserConfigPtr Users;

    //! Path in filesystem to the internal state.
    TString DataPath;

    //! Path in Cypress with coordination map node, external dictionaries etc.
    TString CypressRootPath;

    //! Log level for internal CH logging.
    TString LogLevel;

    //! External dictionaries.
    std::vector<TDictionaryConfigPtr> Dictionaries;

    //! ClickHouse settings.
    //! Refer to https://clickhouse.yandex/docs/en/operations/settings/settings/ for a complete list.
    //! This map is merged into `users/profiles/default`.
    THashMap<TString, INodePtr> Settings;

    //! Hosts to listen.
    std::vector<TString> ListenHosts;

    //! Paths to geodata stuff.
    TString PathToRegionsHierarchyFile;
    TString PathToRegionsNameFiles;
};

DEFINE_REFCOUNTED_TYPE(TEngineConfig);

////////////////////////////////////////////////////////////////////////////////

class TClickHouseServerBootstrapConfig
    : public TServerConfig
{
public:
    NApi::NNative::TConnectionConfigPtr ClusterConnection;

    TSlruCacheConfigPtr ClientCache;

    //! Authorization settings.
    bool ValidateOperationAccess;
    TDuration OperationAclUpdatePeriod;

    TEngineConfigPtr Engine;

    //! User for communication with YT.
    TString User;

    TDuration ProfilingPeriod;

    TClickHouseServerBootstrapConfig()
    {
        RegisterParameter("cluster_connection", ClusterConnection);

        RegisterParameter("client_cache", ClientCache)
            .DefaultNew();

        RegisterParameter("validate_operation_access", ValidateOperationAccess)
            .Default(true);
        RegisterParameter("operation_acl_update_period", OperationAclUpdatePeriod)
            .Default(TDuration::Minutes(1));

        RegisterParameter("user", User)
            .Default("yt-clickhouse");

        RegisterParameter("engine", Engine)
            .DefaultNew();

        RegisterParameter("profiling_period", ProfilingPeriod)
            .Default(TDuration::Seconds(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TClickHouseServerBootstrapConfig);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
