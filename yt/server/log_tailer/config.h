#pragma once

#include "public.h"

#include <yt/server/lib/misc/config.h>

#include <yt/ytlib/api/native/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NLogTailer {

////////////////////////////////////////////////////////////////////////////////

class TLogRotationConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enable;
    TDuration RotationPeriod;
    int LogSegmentCount;

    //! Pid of the process writing logs.
    //! During the log rotation SIGHUP will be sent to it.
    std::optional<int> LogWriterPid;

    //! Delay between SIGHUP and last read of old log.
    TDuration RotationDelay;

    TLogRotationConfig()
    {
        RegisterParameter("enable", Enable)
            .Default(false);
        RegisterParameter("rotation_period", RotationPeriod)
            .Default();
        RegisterParameter("log_segment_count", LogSegmentCount)
            .Default(5);
        RegisterParameter("log_writer_pid", LogWriterPid)
            .Default();
        RegisterParameter("rotation_delay", RotationDelay)
            .Default(TDuration::MilliSeconds(50));
    }
};

DEFINE_REFCOUNTED_TYPE(TLogRotationConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogFileConfig
    : public NYTree::TYsonSerializable
{
public:
    TString Path;
    std::vector<NYTree::TYPath> TablePaths;

    TLogFileConfig()
    {
        RegisterParameter("path", Path)
            .Default();
        RegisterParameter("table_paths", TablePaths)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TLogFileConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogWriterLivenessCheckerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enable;

    TDuration LivenessCheckPeriod;

    TLogWriterLivenessCheckerConfig()
    {
        RegisterParameter("enable", Enable)
            .Default(false);

        RegisterParameter("liveness_check_period", LivenessCheckPeriod)
            .Default();

        RegisterPostprocessor([&] {
            if (Enable && !LivenessCheckPeriod) {
                THROW_ERROR_EXCEPTION("Log writer liveness checker is enabled while liveness_check_period is not set");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TLogWriterLivenessCheckerConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogTailerConfig
    : public NYTree::TYsonSerializable
{
public:
    TLogRotationConfigPtr LogRotation;

    std::vector<TLogFileConfigPtr> LogFiles;

    TLogWriterLivenessCheckerConfigPtr LogWriterLivenessChecker;

    TDuration ReadPeriod;

    //! Read buffer size in bytes.
    i64 ReadBufferSize;
    i64 MaxRecordsPerTransaction;
    i64 MaxRecordsInBuffer;

    TLogTailerConfig()
    {
        RegisterParameter("log_rotation", LogRotation)
            .DefaultNew();

        RegisterParameter("log_files", LogFiles)
            .Default();

        RegisterParameter("log_writer_liveness_checker", LogWriterLivenessChecker)
            .Default();

        RegisterParameter("read_period", ReadPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("read_buffer_size", ReadBufferSize)
            .Default(64_KB);

        RegisterParameter("max_records_per_transaction", MaxRecordsPerTransaction)
            .Default(100);

        RegisterParameter("max_records_in_buffer", MaxRecordsInBuffer)
            .Default(1000);
    }
};

DEFINE_REFCOUNTED_TYPE(TLogTailerConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogTailerBootstrapConfig
    : public TServerConfig
{
public:
    TLogTailerConfigPtr LogTailer;

    TString ClusterUser;

    NApi::NNative::TConnectionConfigPtr ClusterConnection;

    TLogTailerBootstrapConfig()
    {
        RegisterParameter("log_tailer", LogTailer);

        RegisterParameter("cluster_user", ClusterUser)
            .Default("yt-log-tailer");

        RegisterParameter("cluster_connection", ClusterConnection);
    }
};

DEFINE_REFCOUNTED_TYPE(TLogTailerBootstrapConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogTailer
