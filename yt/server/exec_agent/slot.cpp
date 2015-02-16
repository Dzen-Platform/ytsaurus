﻿#include "stdafx.h"
#include "slot.h"
#include "private.h"
#include "config.h"

#include <ytlib/cgroup/cgroup.h>

#include <core/logging/log_manager.h>

#include <core/misc/proc.h>
#include <core/misc/string.h>

#include <core/bus/config.h>

#include <core/ytree/yson_producer.h>

namespace NYT {
namespace NExecAgent {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

Stroka GetSlotProcessGroup(int slotId)
{
    return "slot" + ToString(slotId);
}

////////////////////////////////////////////////////////////////////////////////

TSlot::TSlot(
    TSlotManagerConfigPtr config,
    const Stroka& path,
    const Stroka& nodeId,
    int slotIndex,
    TNullable<int> userId)
    : IsFree_(true)
    , IsClean_(true)
    , Path_(path)
    , NodeId_(nodeId)
    , SlotIndex_(slotIndex)
    , UserId_(userId)
    , SlotThread_(New<TActionQueue>(Format("ExecSlot:%v", slotIndex)))
    , ProcessGroup_("freezer", GetSlotProcessGroup(slotIndex))
    , NullCGroup_()
    , Logger(ExecAgentLogger)
    , Config_(config)
{
    Logger.AddTag("Slot: %v", SlotIndex_);
}

void TSlot::Initialize()
{
    if (Config_->EnableCGroups) {
        try {
            ProcessGroup_.EnsureExistance();
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Failed to create process group %Qv",
                ProcessGroup_.GetFullPath());
        }

#ifdef _linux_
        try {
            NCGroup::RunKiller(ProcessGroup_.GetFullPath());
        } catch (const std::exception& ex) {
            // ToDo(psushin): think about more complex logic of handling fs errors.
            LOG_FATAL(ex, "Failed to clean process group %v",
                ProcessGroup_.GetFullPath());
        }
#endif
    }

    DoResetProcessGroup();

    try {
        NFS::ForcePath(Path_, 0755);
        SandboxPath_ = NFS::CombinePaths(Path_, "sandbox");
        DoCleanSandbox();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to create slot directory %v",
            Path_) << ex;
    }

    try {
        DoCleanProcessGroups();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to clean slot cgroups")
            << ex;
    }
}

void TSlot::Acquire()
{
    IsFree_ = false;
}

bool TSlot::IsFree() const
{
    return IsFree_;
}

TNullable<int> TSlot::GetUserId() const
{
    return UserId_;
}

const NCGroup::TNonOwningCGroup& TSlot::GetProcessGroup() const
{
    return Config_->EnableCGroups ? ProcessGroup_ : NullCGroup_;
}

std::vector<Stroka> TSlot::GetCGroupPaths() const
{
    std::vector<Stroka> result;
    if (Config_->EnableCGroups) {
        auto subgroupName = GetSlotProcessGroup(SlotIndex_);

        for (const auto& type : NCGroup::GetSupportedCGroups()) {
            NCGroup::TNonOwningCGroup group(type, subgroupName);
            result.push_back(group.GetFullPath());
        }
        result.push_back(ProcessGroup_.GetFullPath());
    }
    return result;
}

NBus::TTcpBusServerConfigPtr TSlot::GetRpcServerConfig() const
{
    Stroka unixDomainName = Format("%v-job-proxy-%v", NodeId_, SlotIndex_);
    return NBus::TTcpBusServerConfig::CreateUnixDomain(unixDomainName);
}


void TSlot::DoCleanSandbox()
{
    try {
        if (NFS::Exists(SandboxPath_)) {
            if (UserId_.HasValue()) {
                RunCleaner(SandboxPath_);
            } else {
                NFS::RemoveRecursive(SandboxPath_);
            }
        }
        IsClean_ = true;
    } catch (const std::exception& ex) {
        auto wrappedError = TError("Failed to clean sandbox directory %v",
            SandboxPath_)
            << ex;
        LOG_ERROR(wrappedError);
        THROW_ERROR wrappedError;
    }
}

void TSlot::DoCleanProcessGroups()
{
    try {
        for (const auto& path : GetCGroupPaths()) {
            NCGroup::TNonOwningCGroup group(path);
            group.RemoveAllSubcgroups();
        }
    } catch (const std::exception& ex) {
        auto wrappedError = TError("Failed to clean slot subcgroups for slot %v",
            SlotIndex_) << ex;
        LOG_ERROR(wrappedError);
        THROW_ERROR wrappedError;
    }
}

void TSlot::DoResetProcessGroup()
{
    if (Config_->EnableCGroups) {
        ProcessGroup_.Unlock();
    }
}

void TSlot::Clean()
{
    try {
        DoCleanProcessGroups();
        DoCleanSandbox();
    } catch (const std::exception& ex) {
        LOG_FATAL("%v", ex.what());
    }
}

void TSlot::Release()
{
    YCHECK(IsClean_);

    DoResetProcessGroup();

    IsFree_ = true;
}

void TSlot::InitSandbox()
{
    YCHECK(!IsFree_);

    try {
        NFS::ForcePath(SandboxPath_, 0777);
    } catch (const std::exception& ex) {
        LogErrorAndExit(TError("Failed to create sandbox directory %Qv", SandboxPath_) << ex);
    }

    LOG_INFO("Created slot sandbox directory %Qv", SandboxPath_);

    IsClean_ = false;
}

void TSlot::MakeLink(
    const Stroka& targetPath,
    const Stroka& linkName,
    bool isExecutable) noexcept
{
    auto linkPath = NFS::CombinePaths(SandboxPath_, linkName);
    try {
        {
            // Take exclusive lock in blocking fashion to ensure that no
            // forked process is holding an open descriptor to the target file.
            TFile file(targetPath, RdOnly | CloseOnExec);
            file.Flock(LOCK_EX);
        }

        NFS::MakeSymbolicLink(targetPath, linkPath);
        NFS::SetExecutableMode(linkPath, isExecutable);
    } catch (const std::exception& ex) {
        // Occured IO error in the slot, restart node immediately.
        LogErrorAndExit(TError(
            "Failed to create a symlink in the slot %Qv (LinkPath: %Qv, TargetPath: %Qv, IsExecutable: %lv)",
            SandboxPath_,
            linkPath,
            targetPath,
            isExecutable)
            << ex);
    }
}

void TSlot::LogErrorAndExit(const TError& error)
{
    LOG_ERROR(error);
    NLog::TLogManager::Get()->Shutdown();
    _exit(1);
}

void TSlot::MakeFile(
    const Stroka& fileName,
    std::function<void (TOutputStream*)> dataProducer,
    bool isExecutable)
{
    auto path = NFS::CombinePaths(SandboxPath_, fileName);

    auto error = TError("Failed to create a file in the slot %Qv (FileName: %Qv, IsExecutable: %lv)",
        path,
        fileName,
        isExecutable);

    try {
        // NB! Races are possible between file creation and call to flock.
        // Unfortunately in Linux we cannot make it atomically.
        TFile file(path, CreateAlways | CloseOnExec);
        file.Flock(LOCK_EX | LOCK_NB);
        TFileOutput fileOutput(file);

        // Producer may throw non IO-related exceptions, that we do not handle.
        dataProducer(&fileOutput);
        NFS::SetExecutableMode(path, isExecutable);
    } catch (const TFileError& ex) {
        LogErrorAndExit(error << TError(ex.what()));
    }

    try {
        // Take exclusive lock in blocking fashion to ensure that no
        // forked process is holding an open descriptor.
        TFile file(path, RdOnly | CloseOnExec);
        file.Flock(LOCK_EX);
    } catch (const std::exception& ex) {
        LogErrorAndExit(error << ex);
    }

}

const Stroka& TSlot::GetWorkingDirectory() const
{
    return Path_;
}

IInvokerPtr TSlot::GetInvoker()
{
    return SlotThread_->GetInvoker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
