#ifdef __linux__

#include "instance.h"

#include "porto_executor.h"
#include "private.h"

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/error.h>

#include <yt/contrib/portoapi/libporto.hpp>

#include <util/string/cast.h>

#include <initializer_list>

namespace NYT::NContainers {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

// Porto passes command string to wordexp, where quota (') symbol
// is delimiter. So we must replace it with concatenation ('"'"').
static TString EscapeForWordexp(const char* in)
{
    TString buffer;
    while (*in) {
        if (*in == '\'') {
            buffer.append(R"('"'"')");
        } else {
            buffer.append(*in);
        }
        in++;
    }
    return buffer;
}

using TPortoStatRule = std::pair<TString, TCallback<i64(const TString& input)>>;

static i64 Extract(const TString& input, const TString& pattern, const TString& terminator = "\n")
{
    auto start = input.find(pattern) + pattern.length();
    auto end = input.find(terminator, start);
    return std::stol(input.substr(start, (end == input.npos) ? end : end - start));
}

static i64 ExtractSum(const TString& input, const TString& pattern, const TString& delimiter, const TString& terminator = "\n")
{
    i64 sum = 0;
    TString::size_type pos = 0;
    while (pos < input.length()) {
        pos = input.find(pattern, pos);
        if (pos == input.npos) {
            break;
        }
        pos += pattern.length();

        pos = input.find(delimiter, pos);
        if (pos == input.npos) {
            break;
        }

        pos++;
        auto end = input.find(terminator, pos);
        sum += std::stol(input.substr(pos, (end == input.npos) ? end : end - pos));
    }
    return sum;
}

////////////////////////////////////////////////////////////////////////////////

class TPortoInstance
    : public IInstance
{
public:
    static IInstancePtr Create(const TString& name, IPortoExecutorPtr executor, bool autoDestroy)
    {
        auto error = WaitFor(executor->CreateContainer(name));
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Unable to create container");
        return New<TPortoInstance>(name, executor, autoDestroy);
    }

    static IInstancePtr GetSelf(IPortoExecutorPtr executor)
    {
        return New<TPortoInstance>("self", executor, false);
    }

    static IInstancePtr GetInstance(IPortoExecutorPtr executor, const TString& name)
    {
        return New<TPortoInstance>(name, executor, false);
    }

    ~TPortoInstance()
    {
        // We can't wait here, but even if this request fails
        // it is not a big issue - porto has its own GC.
        if (!Destroyed_ && AutoDestroy_) {
            Executor_->DestroyContainer(Name_);
        }
    }

    virtual void SetStdIn(const TString& inputPath) override
    {
        SetProperty("stdin_path", inputPath);
    }

    virtual void SetStdOut(const TString& outPath) override
    {
        SetProperty("stdout_path", outPath);
    }

    virtual void SetStdErr(const TString& errorPath) override
    {
        SetProperty("stderr_path", errorPath);
    }

    virtual void SetCwd(const TString& cwd) override
    {
        SetProperty("cwd", cwd);
    }

    virtual void SetCoreDumpHandler(const TString& handler) override
    {
        SetProperty("core_command", handler);
    }

    virtual void Kill(int signal) override
    {
        auto error = WaitFor(Executor_->Kill(Name_, signal));
        // Killing already finished process is not an error.
        if (error.FindMatching(EContainerErrorCode::InvalidState)) {
            return;
        }
        if (!error.IsOK()) {
            THROW_ERROR_EXCEPTION("Failed to send signal to porto instance")
                << TErrorAttribute("signal", signal)
                << TErrorAttribute("container", Name_)
                << error;
        }
    }

    virtual void SetRoot(const TRootFS& rootFS) override
    {
        HasRoot_ = true;
        SetProperty("root", rootFS.RootPath);
        SetProperty("root_readonly", TString(FormatBool(rootFS.IsRootReadOnly)));

        TStringBuilder builder;
        for (const auto& bind : rootFS.Binds) {
            builder.AppendString(bind.SourcePath);
            builder.AppendString(" ");
            builder.AppendString(bind.TargetPath);
            builder.AppendString(" ");
            builder.AppendString(bind.IsReadOnly ? "ro" : "rw");
            builder.AppendString(" ; ");
        }

        SetProperty("bind", builder.Flush());
    }

    virtual void SetDevices(const std::vector<TDevice>& devices) override
    {
        TStringBuilder builder;
        for (const auto& device : devices) {
            builder.AppendString(device.DeviceName);
            builder.AppendString(" ");
            if (device.Enabled) {
                builder.AppendString("rw");
            } else {
                builder.AppendString("-");
            }
            builder.AppendString(" ; ");
        }

        SetProperty("devices", builder.Flush());
    }

    virtual bool HasRoot() const override
    {
        return HasRoot_;
    }

    virtual void Destroy() override
    {
        WaitFor(Executor_->DestroyContainer(Name_))
            .ThrowOnError();
        Destroyed_ = true;
    }

    virtual void Stop() override
    {
        WaitFor(Executor_->Stop(Name_))
            .ThrowOnError();
    }

    virtual TUsage GetResourceUsage(const std::vector<EStatField>& fields) const override
    {
        std::vector<TString> properties;
        TUsage result;
        try {
            for (auto field : fields) {
                const auto& rules = StatRules_.at(field);
                properties.push_back(rules.first);
            }
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Unknown resource field requested");
            THROW_ERROR_EXCEPTION("Unknown resource field requested")
                << TErrorAttribute("container", Name_)
                << ex;
        }

        auto response = WaitFor(Executor_->GetProperties(Name_, properties))
            .ValueOrThrow();

        for (auto field : fields) {
            const auto& rules = StatRules_.at(field);
            TErrorOr<ui64>& record = result[field];
            try {
                auto data = response.at(rules.first)
                    .ValueOrThrow();
                record = rules.second(data);
            } catch (const std::exception& ex) {
                record = TError("Unable to get %Qv from porto", rules.first)
                    << TErrorAttribute("container", Name_)
                    << ex;
            }
        }
        return result;
    }

    virtual TResourceLimits GetResourceLimits() const override
    {
        std::vector<TString> properties;
        properties.push_back("memory_limit");
        properties.push_back("cpu_limit");

        auto responseOrError = WaitFor(Executor_->GetProperties(Name_, properties));
        THROW_ERROR_EXCEPTION_IF_FAILED(responseOrError, "Failed to get porto container resource limits");

        const auto& response = responseOrError.Value();
        const auto& memoryLimitRsp = response.at("memory_limit");

        THROW_ERROR_EXCEPTION_IF_FAILED(memoryLimitRsp, "Failed to get memory limit from porto");

        i64 memoryLimit;

        if (!TryFromString<i64>(memoryLimitRsp.Value(), memoryLimit)) {
            THROW_ERROR_EXCEPTION("Failed to parse memory limit value from porto")
                << TErrorAttribute("memory_limit", memoryLimitRsp.Value());
        }

        const auto& cpuLimitRsp = response.at("cpu_limit");

        THROW_ERROR_EXCEPTION_IF_FAILED(cpuLimitRsp, "Failed to get cpu limit from porto");

        double cpuLimit;

        YT_VERIFY(cpuLimitRsp.Value().EndsWith('c'));
        auto cpuLimitValue = TStringBuf(cpuLimitRsp.Value().begin(), cpuLimitRsp.Value().size() - 1);
        if (!TryFromString<double>(cpuLimitValue, cpuLimit)) {
            THROW_ERROR_EXCEPTION("Failed to parse cpu limit value from porto")
                << TErrorAttribute("cpu_limit", cpuLimitRsp.Value());
        }

        return TResourceLimits{cpuLimit, memoryLimit};
    }

    virtual TResourceLimits GetResourceLimitsRecursive() const override
    {
        static const TString Prefix("/porto");

        auto resourceLimits = GetResourceLimits();

        auto absoluteName = GetAbsoluteName();

        auto slashPosition = absoluteName.rfind('/');
        auto parentName = absoluteName.substr(0, slashPosition);

        if (parentName != Prefix) {
            YT_VERIFY(parentName.length() > Prefix.length());
            auto parent = GetInstance(Executor_, parentName);
            auto parentLimits = parent->GetResourceLimitsRecursive();

            if (resourceLimits.Cpu == 0 || (parentLimits.Cpu < resourceLimits.Cpu && parentLimits.Cpu > 0)) {
                resourceLimits.Cpu = parentLimits.Cpu;
            }

            if (resourceLimits.Memory == 0 || (parentLimits.Memory < resourceLimits.Memory && parentLimits.Memory > 0)) {
                resourceLimits.Memory = parentLimits.Memory;
            }
        }

        return resourceLimits;
    }

    virtual TString GetAbsoluteName() const override
    {
        auto properties = WaitFor(Executor_->GetProperties(
            Name_,
            std::vector<TString>{"absolute_name"}))
            .ValueOrThrow();

        return properties.at("absolute_name")
             .ValueOrThrow();
    }

    virtual void SetCpuShare(double cores) override
    {
        SetProperty("cpu_guarantee", ToString(cores) + "c");
    }

    virtual void SetCpuLimit(double cores) override
    {
        SetProperty("cpu_limit", ToString(cores) + "c");
    }

    virtual void SetIsolate() override
    {
        Isolate_ = true;
    }

    virtual void EnableMemoryTracking() override
    {
        RequireMemoryController_ = true;
    }

    virtual void SetMemoryGuarantee(i64 memoryGuarantee) override
    {
        SetProperty("memory_guarantee", ToString(memoryGuarantee));
        RequireMemoryController_ = true;
    }

    virtual void SetIOWeight(double weight) override
    {
        SetProperty("io_weight", ToString(weight));
    }

    virtual void SetIOThrottle(i64 operations) override
    {
        SetProperty("io_ops_limit", ToString(operations));
    }

    virtual void SetUser(const TString& user) override
    {
        User_ = user;
    }

    virtual TString GetName() const override
    {
        return Name_;
    }

    virtual pid_t GetPid() const override
    {
        auto pid = WaitFor(Executor_->GetProperties(Name_, std::vector<TString>{"root_pid"}))
            .ValueOrThrow();
        return std::stoi(pid.at("root_pid")
            .ValueOrThrow());
    }

    virtual TFuture<int> Exec(
        const std::vector<const char*>& argv,
        const std::vector<const char*>& env) override
    {
        TString command;

        for (auto arg : argv) {
            command += TString("'") + EscapeForWordexp(arg) + TString("'");
            command += " ";
        }

        YT_LOG_DEBUG("Executing porto container (Command: %v)", command);

        if (!User_) {
            // NB(psushin): Make sure subcontainer starts with the same user.
            // For unknown reason in the cloud we've seen user_job containers with user=loadbase.
            SetProperty("user", ToString(::getuid()));
        } else {
            SetProperty("user", User_);
        }

        // Enable core dumps for all container instances.
        SetProperty("ulimit", "core: unlimited");
        TString controllers = "freezer;cpu;cpuacct;net_cls;blkio;devices;pids";
        if (RequireMemoryController_) {
            controllers += ";memory";
        }
        SetProperty("controllers", controllers);
        SetProperty("enable_porto", Isolate_ ? "isolate" : "full");
        SetProperty("isolate", Isolate_ ? "true" : "false");
        SetProperty("command", command);

        TStringBuilder envBuilder;
        for (auto arg : env) {
            envBuilder.AppendString(arg);
            envBuilder.AppendChar(';');
        }
        SetProperty("env", envBuilder.Flush());

        // Wait for all pending actions - do not start real execution if
        // preparation has failed
        WaitForActions().ThrowOnError();
        TFuture<void> startAction = Executor_->Start(Name_);
        // Wait for starting process - here we get error if exec has failed
        // i.e. no such file, execution bit, etc
        // In theory it is not necessarily to wait here, but in this case
        // error handling will be more difficult.
        auto error = WaitFor(startAction);
        if (!error.IsOK()) {
            THROW_ERROR_EXCEPTION(EErrorCode::FailedToStartContainer, "Unable to start container")
                << error;
        }

        return Executor_->AsyncPoll(Name_);
    }

private:
    const TString Name_;
    mutable IPortoExecutorPtr Executor_;
    std::vector<TFuture<void>> Actions_;
    static const std::map<EStatField, TPortoStatRule> StatRules_;
    const NLogging::TLogger Logger;
    const bool AutoDestroy_;
    bool Destroyed_ = false;
    bool HasRoot_ = false;
    bool Isolate_ = false;
    bool RequireMemoryController_ = false;
    TString User_;

    TPortoInstance(
        const TString& name,
        IPortoExecutorPtr executor,
        bool autoDestroy)
        : Name_(name)
        , Executor_(executor)
        , Logger(NLogging::TLogger(ContainersLogger)
            .AddTag("Container: %v", Name_))
        , AutoDestroy_(autoDestroy)
    { }

    void SetProperty(const TString& key, const TString& value)
    {
        Actions_.push_back(Executor_->SetProperty(Name_, key, value));
    }

    TError WaitForActions()
    {
        auto error = WaitFor(Combine(Actions_));
        Actions_.clear();
        return error;
    }

    DECLARE_NEW_FRIEND();
};

const std::map<EStatField, TPortoStatRule> TPortoInstance::StatRules_ = {
    { EStatField::CpuUsageUser,    { "cpu_usage",
        BIND([](const TString& in) { return std::stol(in);                     } ) } },
    { EStatField::CpuUsageSystem,  { "cpu_usage_system",
        BIND([](const TString& in) { return std::stol(in);                     } ) } },
    { EStatField::CpuStolenTime,   { "cpu_wait_time",
        BIND([](const TString& in) { return std::stol(in);                     } ) } },
    { EStatField::Rss,             { "memory.stat",
        BIND([](const TString& in) { return Extract(in, "rss");                } ) } },
    { EStatField::MappedFiles,     { "memory.stat",
        BIND([](const TString& in) { return Extract(in, "mapped_file");        } ) } },
    { EStatField::IOOperations,    { "io_ops",
        BIND([](const TString& in) { return ExtractSum(in, "sd", ":", ";");    } ) } },
    { EStatField::IOReadByte,      { "io_read",
        BIND([](const TString& in) { return ExtractSum(in, "sd", ":", ";");    } ) } },
    { EStatField::IOWriteByte,      { "io_write",
        BIND([](const TString& in) { return ExtractSum(in, "sd", ":", ";");    } ) } },
    { EStatField::MaxMemoryUsage,  { "memory.max_usage_in_bytes",
        BIND([](const TString& in) { return std::stol(in);                     } ) } },
    { EStatField::MajorFaults,     { "major_faults",
        BIND([](const TString& in) { return std::stol(in);                     } ) } }
};

////////////////////////////////////////////////////////////////////////////////

IInstancePtr CreatePortoInstance(const TString& name, IPortoExecutorPtr executor, bool autoDestroy)
{
    return TPortoInstance::Create(name, executor, autoDestroy);
}

IInstancePtr GetSelfPortoInstance(IPortoExecutorPtr executor)
{
    return TPortoInstance::GetSelf(executor);
}

IInstancePtr GetPortoInstance(IPortoExecutorPtr executor, const TString& name)
{
    return TPortoInstance::GetInstance(executor, name);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NContainers

#endif
