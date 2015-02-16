﻿#pragma once

#include "public.h"

#include <core/misc/fs.h>

#include <core/concurrency/action_queue.h>

#include <core/logging/log.h>

#include <core/bus/public.h>

#include <ytlib/cgroup/cgroup.h>

#include <ytlib/formats/format.h>

#include <util/stream/file.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

class TSlot
    : public TRefCounted
{
public:
    TSlot(
        TSlotManagerConfigPtr config,
        const Stroka& path,
        const Stroka& nodeId,
        int slotIndex,
        TNullable<int> userId);

    void Initialize();

    bool IsFree() const;
    TNullable<int> GetUserId() const;
    const NCGroup::TNonOwningCGroup& GetProcessGroup() const;
    std::vector<Stroka> GetCGroupPaths() const;

    NBus::TTcpBusServerConfigPtr GetRpcServerConfig() const;
    NBus::TTcpBusClientConfigPtr GetRpcClientConfig() const;

    void Acquire();
    void InitSandbox();
    void Clean();
    void Release();

    IInvokerPtr GetInvoker();

    //! Creates a symbolic link #linkName for #targetPath in the sandbox.
    void MakeLink(
        const Stroka& targetPath,
        const Stroka& linkName,
        bool isExecutable) noexcept;

    //! Writes to a file #fileName in the sandbox; #dataProducer provides the data.
    void MakeFile(
        const Stroka& fileName,
        std::function<void (TOutputStream*)> dataProducer,
        bool isExecutable = false);

    const Stroka& GetWorkingDirectory() const;

private:
    volatile bool IsFree_;
    bool IsClean_;

    Stroka Path_;
    Stroka NodeId_;
    int SlotIndex_;
    TNullable<int> UserId_;

    Stroka SandboxPath_;

    NConcurrency::TActionQueuePtr SlotThread_;

    NCGroup::TNonOwningCGroup ProcessGroup_;
    NCGroup::TNonOwningCGroup NullCGroup_;

    NLog::TLogger Logger;
    TSlotManagerConfigPtr Config_;


    void DoCleanSandbox();
    void DoCleanProcessGroups();
    void DoResetProcessGroup();

    void LogErrorAndExit(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
