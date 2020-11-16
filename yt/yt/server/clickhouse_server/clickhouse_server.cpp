#include "clickhouse_server.h"

#include "clickhouse_config.h"
#include "config_repository.h"
#include "logger.h"
#include "http_handler.h"
#include "tcp_handler.h"
#include "poco_config.h"
#include "host.h"
#include "helpers.h"
#include "caching_profiler.h"
#include "clickhouse_singletons.h"
#include "format.h"

#include <yt/core/misc/fs.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/coroutine.h>

#include <yt/core/profiling/profile_manager.h>

#include <Server/IServer.h>

#include <Access/AccessControlManager.h>
#include <Access/MemoryAccessStorage.h>
#include <Common/ClickHouseRevision.h>
#include <Common/MemoryTracker.h>
#include <Databases/DatabaseMemory.h>
#include <Interpreters/AsynchronousMetrics.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/QueryLog.h>
#include <Interpreters/executeQuery.h>
#include <Storages/StorageMemory.h>
#include <Storages/System/StorageSystemAsynchronousMetrics.h>
#include <Storages/System/StorageSystemDictionaries.h>
#include <Storages/System/StorageSystemMetrics.h>
#include <Storages/System/StorageSystemProcesses.h>
#include <Storages/System/attachSystemTables.h>

#include <Poco/DirectoryIterator.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/TCPServer.h>
#include <Poco/ThreadPool.h>
#include <Poco/Util/LayeredConfiguration.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;

static const auto& Logger = ClickHouseYtLogger;

////////////////////////////////////////////////////////////////////////////////

class TClickHouseServer
    : public DB::IServer
    , public IClickHouseServer
    , public NProfiling::ISensorProducer
{
public:
    TClickHouseServer(
        THost* host,
        TClickHouseConfigPtr config)
        : Host_(std::move(host))
        , Config_(config)
        , SharedContext_(DB::Context::createShared())
        , ServerContext_(std::make_unique<DB::Context>(DB::Context::createGlobal(SharedContext_.get())))
        , LayeredConfig_(ConvertToLayeredConfig(ConvertToNode(Config_)))
    {
        SetupLogger();

        // NB: under debug build this method does not fit in regular fiber stack
        // due to force inlining stack bloat: https://max42.at.yandex-team.ru/103.
        TCoroutine<void()> coroutine(BIND([&] (TCoroutine<void()>& /* self */) {
            SetupContext();
        }), EExecutionStackKind::Large);
        coroutine.Run();
        YT_VERIFY(coroutine.IsCompleted());

        WarmupDictionaries();
    }

    // IClickHouseServer overrides:

    virtual void Start() override
    {
        SetupServers();

        ClickHouseNativeProfiler.AddProducer("", MakeStrong(this));

        for (auto& server : Servers_) {
            server->start();
        }
    }

    virtual void Stop() override
    {
        Cancelled_ = true;

        for (auto& server : Servers_) {
            if (auto httpPtr = dynamic_cast<Poco::Net::HTTPServer*>(server.get())) {
                // Special method of HTTP Server, will break all active connections.
                httpPtr->stopAll(true);
            } else {
                server->stop();
            }
        }
    }

    DB::Context* GetContext() override
    {
        return ServerContext_.get();
    }

    // DB::Server overrides:

    Poco::Logger& logger() const override
    {
        return Poco::Logger::root();
    }

    Poco::Util::LayeredConfiguration& config() const override
    {
        return *const_cast<Poco::Util::LayeredConfiguration*>(LayeredConfig_.get());
    }

    DB::Context& context() const override
    {
        return *ServerContext_;
    }

    bool isCancelled() const override
    {
        return Cancelled_;
    }

private:
    THost* Host_;
    const TClickHouseConfigPtr Config_;
    DB::SharedContextHolder SharedContext_;
    std::unique_ptr<DB::Context> ServerContext_;

    // Poco representation of Config_.
    Poco::AutoPtr<Poco::Util::LayeredConfiguration> LayeredConfig_;

    Poco::AutoPtr<Poco::Channel> LogChannel;

    std::unique_ptr<DB::AsynchronousMetrics> AsynchronousMetrics_;

    std::unique_ptr<Poco::ThreadPool> ServerPool_;
    std::vector<std::unique_ptr<Poco::Net::TCPServer>> Servers_;

    std::atomic<bool> Cancelled_ { false };

    std::shared_ptr<DB::IDatabase> SystemDatabase_;

    ext::scope_guard DictionaryGuard_;

    void SetupLogger()
    {
        LogChannel = CreateLogChannel(ClickHouseNativeLogger);

        auto& rootLogger = Poco::Logger::root();
        rootLogger.close();
        rootLogger.setChannel(LogChannel);
        rootLogger.setLevel(Config_->LogLevel);
    }

    void SetupContext()
    {
        YT_LOG_INFO("Setting up context");

        ServerContext_->makeGlobalContext();
        ServerContext_->setApplicationType(DB::Context::ApplicationType::SERVER);
        ServerContext_->setConfig(LayeredConfig_);
        ServerContext_->setUsersConfig(ConvertToPocoConfig(ConvertToNode(Config_->Users)));

        RegisterClickHouseSingletons();

        CurrentMetrics::set(CurrentMetrics::Revision, ClickHouseRevision::getVersionRevision());
        CurrentMetrics::set(CurrentMetrics::VersionInteger, ClickHouseRevision::getVersionInteger());

        // Initialize DateLUT early, to not interfere with running time of first query.
        YT_LOG_DEBUG("Initializing DateLUT");
        DateLUT::setDefaultTimezone(Config_->Timezone.value());
        DateLUT::instance();
        YT_LOG_DEBUG("DateLUT initialized (TimeZone: %v)", DateLUT::instance().getTimeZone());

        // Limit on total number of concurrently executed queries.
        ServerContext_->getProcessList().setMaxSize(Config_->MaxConcurrentQueries);

        ServerContext_->setDefaultProfiles(*LayeredConfig_);

        YT_LOG_DEBUG("Profiles, processes & uncompressed cache set up");

        NFS::MakeDirRecursive(Config_->DataPath);
        ServerContext_->setPath(Config_->DataPath);

        // This object will periodically calculate asynchronous metrics.
        AsynchronousMetrics_ = std::make_unique<DB::AsynchronousMetrics>(*ServerContext_);

        YT_LOG_DEBUG("Asynchronous metrics set up");

        // Database for system tables.

        YT_LOG_DEBUG("Setting up databases");

        SystemDatabase_ = std::make_shared<DB::DatabaseMemory>(DB::DatabaseCatalog::SYSTEM_DATABASE, *ServerContext_);

        DB::DatabaseCatalog::instance().attachDatabase(DB::DatabaseCatalog::SYSTEM_DATABASE, SystemDatabase_);

        SystemDatabase_->attachTable("processes", DB::StorageSystemProcesses::create("processes"));
        SystemDatabase_->attachTable("metrics", DB::StorageSystemMetrics::create("metrics"));
        SystemDatabase_->attachTable("dictionaries", DB::StorageSystemDictionaries::create("dictionaries"));
        SystemDatabase_->attachTable("asynchronous_metrics", DB::StorageSystemAsynchronousMetrics::create("asynchronous_metrics", *AsynchronousMetrics_));

        DB::attachSystemTablesLocal(*SystemDatabase_);
        Host_->PopulateSystemDatabase(SystemDatabase_.get());

        DB::DatabaseCatalog::instance().attachDatabase("YT", Host_->CreateYtDatabase());
        ServerContext_->setCurrentDatabase("YT");

        auto DatabaseForTemporaryAndExternalTables = std::make_shared<DB::DatabaseMemory>(DB::DatabaseCatalog::TEMPORARY_DATABASE, *ServerContext_);
        DB::DatabaseCatalog::instance().attachDatabase(DB::DatabaseCatalog::TEMPORARY_DATABASE, DatabaseForTemporaryAndExternalTables);

        YT_LOG_DEBUG("Initializing system logs");

        PrepareSystemLogTables();
        ServerContext_->initializeSystemLogs();

        YT_LOG_DEBUG("System logs initialized");

        if (Config_->MaxServerMemoryUsage) {
            total_memory_tracker.setOrRaiseHardLimit(*Config_->MaxServerMemoryUsage);
            total_memory_tracker.setDescription("(total)");
            total_memory_tracker.setMetric(CurrentMetrics::MemoryTracking);
        }

        YT_LOG_DEBUG("Setting up access manager");

        ServerContext_->getAccessControlManager().addStorage(std::make_unique<DB::MemoryAccessStorage>());
        RegisterNewUser(ServerContext_->getAccessControlManager(), InternalRemoteUserName);

        YT_LOG_DEBUG("Adding external dictionaries from config");

        DictionaryGuard_ = ServerContext_->getExternalDictionariesLoader().addConfigRepository(CreateDictionaryConfigRepository(Config_->Dictionaries));

        YT_LOG_DEBUG("Setting chyt custom setting prefix");

        ServerContext_->getAccessControlManager().setCustomSettingsPrefixes(std::vector<std::string>{"chyt_", "chyt."});

        YT_LOG_INFO("Finished setting up context");
    }

    void WarmupDictionaries()
    {
        YT_LOG_INFO("Warming up dictionaries");
        ServerContext_->getEmbeddedDictionaries();
        YT_LOG_INFO("Finished warming up");
    }

    void PrepareSystemLogTables()
    {
        YT_LOG_DEBUG("Preparing query log tables");
        // This log won't actually be serving as log; we use it
        // to extract table creation query and apply it to our two
        // buffer tables implementing in-memory query log with rotation.
        auto log = std::make_shared<DB::QueryLog>(
            *ServerContext_,
            "system",
            "{table_name}",
            Config_->QueryLog->Engine,
            Config_->QueryLog->FlushIntervalMilliseconds);

        auto createTableAst = log->getCreateTableQuery();
        auto createTableQuery = TString(DB::serializeAST(*createTableAst));

        const TString TableNamePlaceholder = "{table_name}";
        const TString UnderlyingTableNamePlaceholder = "{underlying_table_name}";
        const TString DatabasePlaceholder = "{database}";

        auto replace = [&] (TString query, TString placeholder, TString with) {
            auto position = query.find(placeholder);
            YT_VERIFY(position != TString::npos);
            query.replace(position, placeholder.size(), with);
            return query;
        };

        auto createTableQueryNewer = createTableQuery;
        createTableQueryNewer = replace(createTableQueryNewer, TableNamePlaceholder, "query_log");
        createTableQueryNewer = replace(createTableQueryNewer, UnderlyingTableNamePlaceholder, "query_log_older");
        createTableQueryNewer = replace(createTableQueryNewer, DatabasePlaceholder, "system");

        YT_LOG_DEBUG("Creating newer query log table (Query: %v)", createTableQueryNewer);
        DB::executeQuery(createTableQueryNewer, *ServerContext_, true);

        auto createTableQueryOlder = createTableQuery;
        createTableQueryOlder = replace(createTableQueryOlder, TableNamePlaceholder, "query_log_older");
        createTableQueryOlder = replace(createTableQueryOlder, UnderlyingTableNamePlaceholder, "");
        createTableQueryOlder = replace(createTableQueryOlder, DatabasePlaceholder, "");

        YT_LOG_DEBUG("Creating older query log table (Query: %v)", createTableQueryOlder);
        DB::executeQuery(createTableQueryOlder, *ServerContext_, true);
    }

    void SetupServers()
    {
#ifdef _linux_
        YT_LOG_INFO("Setting up servers");

        const auto& settings = ServerContext_->getSettingsRef();

        ServerPool_ = std::make_unique<Poco::ThreadPool>(3, Config_->MaxConnections);

        auto setupSocket = [&] (const std::string& host, UInt16 port) {
            Poco::Net::SocketAddress socketAddress;
            socketAddress = Poco::Net::SocketAddress(host, port);
            Poco::Net::ServerSocket socket(socketAddress);
            socket.setReceiveTimeout(settings.receive_timeout);
            socket.setSendTimeout(settings.send_timeout);

            return socket;
        };


        {
            YT_LOG_INFO("Setting up HTTP server");
            auto socket = setupSocket("::", Config_->HttpPort);

            Poco::Timespan keepAliveTimeout(Config_->KeepAliveTimeout, 0);

            Poco::Net::HTTPServerParams::Ptr httpParams = new Poco::Net::HTTPServerParams();
            httpParams->setTimeout(settings.receive_timeout);
            httpParams->setKeepAliveTimeout(keepAliveTimeout);

            Servers_.emplace_back(std::make_unique<Poco::Net::HTTPServer>(
                CreateHttpHandlerFactory(Host_, *this),
                *ServerPool_,
                socket,
                httpParams));
        }

        {
            YT_LOG_INFO("Setting up TCP server");
            auto socket = setupSocket("::", Config_->TcpPort);

            Servers_.emplace_back(std::make_unique<Poco::Net::TCPServer>(
                CreateTcpHandlerFactory(Host_, *this),
                *ServerPool_,
                socket,
                new Poco::Net::TCPServerParams()));
        }

        YT_LOG_INFO("Servers set up");
#endif
    }

    void Collect(NProfiling::ISensorWriter* writer)
    {
        for (int index = 0; index < static_cast<int>(CurrentMetrics::end()); ++index) {
            const auto* name = CurrentMetrics::getName(index);
            auto value = CurrentMetrics::values[index].load(std::memory_order_relaxed);

            writer->AddGauge("/current_metrics/" + CamelCaseToUnderscoreCase(TString(name)), value);
        }

        for (const auto& [name, value] : AsynchronousMetrics_->getValues()) {
            writer->AddGauge("/asynchronous_metrics/" + CamelCaseToUnderscoreCase(TString(name)), value);
        }

        for (int index = 0; index < static_cast<int>(ProfileEvents::end()); ++index) {
            const auto* name = ProfileEvents::getName(index);
            auto value = ProfileEvents::global_counters[index].load(std::memory_order_relaxed);

            writer->AddCounter("/global_profile_events/" + CamelCaseToUnderscoreCase(TString(name)), value);
        }

        if (Config_->MaxServerMemoryUsage) {
            writer->AddGauge("/memory_limit", *Config_->MaxServerMemoryUsage);
        }

        for (
            auto tableIterator = SystemDatabase_->getTablesIterator(*ServerContext_);
            tableIterator->isValid();
            tableIterator->next())
        {
            auto totalBytes = tableIterator->table()->totalBytes();
            if (!totalBytes) {
                continue;
            }

            writer->PushTag(NProfiling::TTag{"table", tableIterator->name()});
            writer->AddGauge("/system_tables/memory", *totalBytes);
            writer->PopTag();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IClickHouseServerPtr CreateClickHouseServer(
    THost* host,
    TClickHouseConfigPtr config)
{
    return New<TClickHouseServer>(std::move(host), std::move(config));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NClickHouseServer
