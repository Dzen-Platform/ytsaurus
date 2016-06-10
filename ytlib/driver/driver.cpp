#include "driver.h"
#include "command.h"
#include "config.h"
#include "cypress_commands.h"
#include "etc_commands.h"
#include "file_commands.h"
#include "journal_commands.h"
#include "scheduler_commands.h"
#include "table_commands.h"
#include "transaction_commands.h"

#include <yt/ytlib/api/transaction.h>

#include <yt/core/yson/null_consumer.h>

#include <yt/core/misc/sync_cache.h>

#include <yt/core/concurrency/lease_manager.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NYson;
using namespace NRpc;
using namespace NElection;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NScheduler;
using namespace NFormats;
using namespace NSecurityClient;
using namespace NConcurrency;
using namespace NHydra;
using namespace NHive;
using namespace NTabletClient;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DriverLogger;

////////////////////////////////////////////////////////////////////////////////

TDriverRequest::TDriverRequest()
    : ResponseParametersConsumer(GetNullYsonConsumer())
{ }

////////////////////////////////////////////////////////////////////////////////

const TCommandDescriptor IDriver::GetCommandDescriptor(const Stroka& commandName) const
{
    auto descriptor = FindCommandDescriptor(commandName);
    YCHECK(descriptor);
    return descriptor.Get();
}

////////////////////////////////////////////////////////////////////////////////

class TCachedClient
    : public TSyncCacheValueBase<Stroka, TCachedClient>
{
public:
    TCachedClient(
        const Stroka& user,
        IClientPtr client)
        : TSyncCacheValueBase(user)
        , Client_(std::move(client))
    { }

    IClientPtr GetClient()
    {
        return Client_;
    }

private:
    const IClientPtr Client_;
};

class TDriver;
typedef TIntrusivePtr<TDriver> TDriverPtr;

class TDriver
    : public IDriver
    , public TSyncSlruCacheBase<Stroka, TCachedClient>
{
public:
    explicit TDriver(TDriverConfigPtr config)
        : TSyncSlruCacheBase(config->ClientCache)
        , Config(config)
    {
        YCHECK(Config);

        Connection_ = CreateConnection(Config);

        // Register all commands.
#define REGISTER(command, name, inDataType, outDataType, isVolatile, isHeavy) \
        RegisterCommand<command>( \
            TCommandDescriptor{name, EDataType::inDataType, EDataType::outDataType, isVolatile, isHeavy});

        REGISTER(TStartTransactionCommand,     "start_tx",                Null,       Structured, true,  false);
        REGISTER(TPingTransactionCommand,      "ping_tx",                 Null,       Null,       true,  false);
        REGISTER(TCommitTransactionCommand,    "commit_tx",               Null,       Null,       true,  false);
        REGISTER(TAbortTransactionCommand,     "abort_tx",                Null,       Null,       true,  false);

        REGISTER(TCreateCommand,               "create",                  Null,       Structured, true,  false);
        REGISTER(TRemoveCommand,               "remove",                  Null,       Null,       true,  false);
        REGISTER(TSetCommand,                  "set",                     Structured, Null,       true,  false);
        REGISTER(TGetCommand,                  "get",                     Null,       Structured, false, false);
        REGISTER(TListCommand,                 "list",                    Null,       Structured, false, false);
        REGISTER(TLockCommand,                 "lock",                    Null,       Structured, true,  false);
        REGISTER(TCopyCommand,                 "copy",                    Null,       Structured, true,  false);
        REGISTER(TMoveCommand,                 "move",                    Null,       Structured, true,  false);
        REGISTER(TLinkCommand,                 "link",                    Null,       Structured, true,  false);
        REGISTER(TExistsCommand,               "exists",                  Null,       Structured, false, false);
        REGISTER(TConcatenateCommand,          "concatenate",             Null,       Null,       true,  false);

        REGISTER(TWriteFileCommand,            "write_file",              Binary,     Null,       true,  true );
        REGISTER(TReadFileCommand,             "read_file",               Null,       Binary,     false, true );

        REGISTER(TWriteTableCommand,           "write_table",             Tabular,    Null,       true,  true );
        REGISTER(TReadTableCommand,            "read_table",              Null,       Tabular,    false, true );
        REGISTER(TInsertRowsCommand,           "insert_rows",             Tabular,    Null,       true,  true );
        REGISTER(TDeleteRowsCommand,           "delete_rows",             Tabular,    Null,       true,  true);
        REGISTER(TSelectRowsCommand,           "select_rows",             Null,       Tabular,    false, true );
        REGISTER(TLookupRowsCommand,           "lookup_rows",             Tabular,    Tabular,    false, true );

        REGISTER(TMountTableCommand,           "mount_table",             Null,       Null,       true,  false);
        REGISTER(TUnmountTableCommand,         "unmount_table",           Null,       Null,       true,  false);
        REGISTER(TRemountTableCommand,         "remount_table",           Null,       Null,       true,  false);
        REGISTER(TReshardTableCommand,         "reshard_table",           Null,       Null,       true,  false);
        REGISTER(TAlterTableCommand,           "alter_table",             Null,       Null,       true,  false);

        REGISTER(TMergeCommand,                "merge",                   Null,       Structured, true,  false);
        REGISTER(TEraseCommand,                "erase",                   Null,       Structured, true,  false);
        REGISTER(TMapCommand,                  "map",                     Null,       Structured, true,  false);
        REGISTER(TSortCommand,                 "sort",                    Null,       Structured, true,  false);
        REGISTER(TReduceCommand,               "reduce",                  Null,       Structured, true,  false);
        REGISTER(TJoinReduceCommand,           "join_reduce",             Null,       Structured, true,  false);
        REGISTER(TMapReduceCommand,            "map_reduce",              Null,       Structured, true,  false);
        REGISTER(TRemoteCopyCommand,           "remote_copy",             Null,       Structured, true,  false);
        REGISTER(TAbortOperationCommand,       "abort_op",                Null,       Null,       true,  false);
        REGISTER(TSuspendOperationCommand,     "suspend_op",              Null,       Null,       true,  false);
        REGISTER(TResumeOperationCommand,      "resume_op",               Null,       Null,       true,  false);
        REGISTER(TCompleteOperationCommand,    "complete_op",             Null,       Null,       true,  false);

        REGISTER(TParseYPathCommand,           "parse_ypath",             Null,       Structured, false, false);

        REGISTER(TAddMemberCommand,            "add_member",              Null,       Null,       true,  false);
        REGISTER(TRemoveMemberCommand,         "remove_member",           Null,       Null,       true,  false);
        REGISTER(TCheckPermissionCommand,      "check_permission",        Null,       Structured, false, false);

        REGISTER(TWriteJournalCommand,         "write_journal",           Tabular,    Null,       true,  true );
        REGISTER(TReadJournalCommand,          "read_journal",            Null,       Tabular,    false, true );

        REGISTER(TDumpJobContextCommand,       "dump_job_context",        Null,       Null,       true,  false);
        REGISTER(TStraceJobCommand,            "strace_job",              Null,       Structured, false, false);
        REGISTER(TSignalJobCommand,            "signal_job",              Null,       Null,       false, false);
        REGISTER(TAbandonJobCommand,           "abandon_job",             Null,       Null,       false, false);
        REGISTER(TPollJobShellCommand,         "poll_job_shell",          Null,       Structured, true,  false);
        REGISTER(TAbortJobCommand,             "abort_job",               Null,       Null,       false, false);

        REGISTER(TGetVersionCommand,           "get_version",             Null,       Structured, false, false);

#undef REGISTER
    }

    virtual TFuture<void> Execute(const TDriverRequest& request) override
    {
        auto it = Commands.find(request.CommandName);
        if (it == Commands.end()) {
            return MakeFuture(TError(
                "Unknown command %Qv",
                request.CommandName));
        }

        const auto& entry = it->second;

        YCHECK(entry.Descriptor.InputType == EDataType::Null || request.InputStream);
        YCHECK(entry.Descriptor.OutputType == EDataType::Null || request.OutputStream);

        const auto& user = request.AuthenticatedUser;

        auto cachedClient = Find(user);
        if (!cachedClient) {
            TClientOptions options;
            options.User = user;
            cachedClient = New<TCachedClient>(user, Connection_->CreateClient(options));

            TryInsert(cachedClient, &cachedClient);
        }

        auto context = New<TCommandContext>(
            this,
            entry.Descriptor,
            request,
            cachedClient->GetClient());

        auto invoker = entry.Descriptor.IsHeavy
            ? Connection_->GetHeavyInvoker()
            : Connection_->GetLightInvoker();

        return BIND(&TDriver::DoExecute, entry.Execute, context)
            .AsyncVia(invoker)
            .Run();
    }

    virtual const TNullable<TCommandDescriptor> FindCommandDescriptor(const Stroka& commandName) const override
    {
        auto it = Commands.find(commandName);
        if (it == Commands.end()) {
            return Null;
        }
        return it->second.Descriptor;
    }

    virtual const std::vector<TCommandDescriptor> GetCommandDescriptors() const override
    {
        std::vector<TCommandDescriptor> result;
        result.reserve(Commands.size());
        for (const auto& pair : Commands) {
            result.push_back(pair.second.Descriptor);
        }
        return result;
    }

    virtual IConnectionPtr GetConnection() override
    {
        return Connection_;
    }

private:
    class TCommandContext;
    typedef TIntrusivePtr<TCommandContext> TCommandContextPtr;
    typedef TCallback<void(ICommandContextPtr)> TExecuteCallback;

    TDriverConfigPtr Config;

    IConnectionPtr Connection_;

    struct TCommandEntry
    {
        TCommandDescriptor Descriptor;
        TExecuteCallback Execute;
    };

    yhash_map<Stroka, TCommandEntry> Commands;

    struct TTransactionEntry
    {
        ITransactionPtr Transaction;
        TLease Lease;
    };

    TSpinLock TransactionsLock_;
    yhash_map<TTransactionId, TTransactionEntry> Transactions_;


    template <class TCommand>
    void RegisterCommand(const TCommandDescriptor& descriptor)
    {
        TCommandEntry entry;
        entry.Descriptor = descriptor;
        entry.Execute = BIND([] (ICommandContextPtr context) {
            TCommand command;
            auto parameters = context->Request().Parameters;
            Deserialize(command, parameters);
            command.Execute(context);
        });
        YCHECK(Commands.insert(std::make_pair(descriptor.CommandName, entry)).second);
    }

    static void DoExecute(TExecuteCallback executeCallback, TCommandContextPtr context)
    {
        const auto& request = context->Request();

        TError result;
        TRACE_CHILD("Driver", request.CommandName) {
            LOG_INFO("Command started (RequestId: %" PRIx64 ", Command: %v, User: %v)",
                request.Id,
                request.CommandName,
                request.AuthenticatedUser);

            try {
                executeCallback.Run(context);
            } catch (const std::exception& ex) {
                result = TError(ex);
            }
        }

        if (result.IsOK()) {
            LOG_INFO("Command completed (RequestId: %" PRIx64 ", Command: %v, User: %v)",
                request.Id,
                request.CommandName,
                request.AuthenticatedUser);
        } else {
            LOG_INFO(result, "Command failed (RequestId: %" PRIx64 ", Command: %v, User: %v)",
                request.Id,
                request.CommandName,
                request.AuthenticatedUser);
        }

        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }


    void PinTransaction(ITransactionPtr transaction, TDuration timeout)
    {
        const auto& transactionId = transaction->GetId();

        LOG_DEBUG("Pinning transaction (TransactionId: %v, Timeout: %v)",
            transactionId,
            timeout);

        auto lease = TLeaseManager::CreateLease(
            timeout,
            BIND(IgnoreResult(&TDriver::UnpinTransaction), MakeWeak(this), transactionId));

        {
            TGuard<TSpinLock> guard(TransactionsLock_);
            YCHECK(Transactions_.insert(std::make_pair(
                transactionId,
                TTransactionEntry{std::move(transaction), std::move(lease)})).second);
        }
    }

    bool UnpinTransaction(TTransactionId transactionId)
    {
        ITransactionPtr transaction;
        if (transactionId) {
            TGuard<TSpinLock> guard(TransactionsLock_);
            auto it = Transactions_.find(transactionId);
            if (it != Transactions_.end()) {
                transaction = std::move(it->second.Transaction);
                TLeaseManager::CloseLease(std::move(it->second.Lease));
                Transactions_.erase(it);
            }
        }
        if (transaction) {
            LOG_DEBUG("Unpinning transaction (TransactionId: %v)",
                transactionId);
        }
        return static_cast<bool>(transaction);
    }

    ITransactionPtr FindAndTouchTransaction(TTransactionId transactionId)
    {
        ITransactionPtr transaction;
        if (transactionId) {
            TGuard<TSpinLock> guard(TransactionsLock_);
            auto it = Transactions_.find(transactionId);
            if (it != Transactions_.end()) {
                transaction = it->second.Transaction;
                TLeaseManager::RenewLease(it->second.Lease);
            }
        }
        if (transaction) {
            LOG_DEBUG("Touched pinned transaction (TransactionId: %v)",
                transactionId);
        }
        return transaction;
    }

    class TCommandContext
        : public ICommandContext
    {
    public:
        TCommandContext(
            TDriverPtr driver,
            const TCommandDescriptor& descriptor,
            const TDriverRequest& request,
            IClientPtr client)
            : Driver_(driver)
            , Descriptor_(descriptor)
            , Request_(request)
            , Client_(std::move(client))
        { }

        virtual TDriverConfigPtr GetConfig() override
        {
            return Driver_->Config;
        }

        virtual IClientPtr GetClient() override
        {
            return Client_;
        }

        virtual const TDriverRequest& Request() const override
        {
            return Request_;
        }

        virtual const TFormat& GetInputFormat() override
        {
            if (!InputFormat_) {
                InputFormat_ = ConvertTo<TFormat>(Request_.Parameters->GetChild("input_format"));
            }
            return *InputFormat_;
        }

        virtual const TFormat& GetOutputFormat() override
        {
            if (!OutputFormat_) {
                OutputFormat_ = ConvertTo<TFormat>(Request_.Parameters->GetChild("output_format"));
            }
            return *OutputFormat_;
        }

        virtual TYsonString ConsumeInputValue() override
        {
            YCHECK(Request_.InputStream);
            auto syncInputStream = CreateSyncAdapter(Request_.InputStream);

            auto producer = CreateProducerForFormat(
                GetInputFormat(),
                Descriptor_.InputType,
                syncInputStream.get());

            return ConvertToYsonString(producer);
        }

        virtual void ProduceOutputValue(const TYsonString& yson) override
        {
            YCHECK(Request_.OutputStream);
            auto syncOutputStream = CreateSyncAdapter(Request_.OutputStream);

            TBufferedOutput bufferedOutputStream(syncOutputStream.get());

            auto consumer = CreateConsumerForFormat(
                GetOutputFormat(),
                Descriptor_.OutputType,
                &bufferedOutputStream);

            Serialize(yson, consumer.get());

            consumer->Flush();
        }

        virtual void PinTransaction(ITransactionPtr transaction, TDuration timeout) override
        {
            Driver_->PinTransaction(std::move(transaction), timeout);
        }

        virtual bool UnpinTransaction(const TTransactionId& transactionId) override
        {
            return Driver_->UnpinTransaction(transactionId);
        }

        virtual ITransactionPtr FindAndTouchTransaction(const TTransactionId& transactionId) override
        {
            return Driver_->FindAndTouchTransaction(transactionId);
        }

    private:
        const TDriverPtr Driver_;
        const TCommandDescriptor Descriptor_;

        const TDriverRequest Request_;

        TNullable<TFormat> InputFormat_;
        TNullable<TFormat> OutputFormat_;

        IClientPtr Client_;

    };
};

////////////////////////////////////////////////////////////////////////////////

IDriverPtr CreateDriver(TDriverConfigPtr config)
{
    return New<TDriver>(config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

