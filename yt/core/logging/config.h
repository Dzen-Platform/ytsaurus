#pragma once

#include "public.h"

#include <yt/core/ytree/public.h>
#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

class TWriterConfig
    : public NYTree::TYsonSerializable
{
public:
    EWriterType Type;
    TString FileName;

    TWriterConfig()
    {
        RegisterParameter("type", Type);
        RegisterParameter("file_name", FileName)
            .Default();

        RegisterValidator([&] () {
            if (Type == EWriterType::File && FileName.empty()) {
                THROW_ERROR_EXCEPTION("Missing \"file_name\" attribute for \"file\" writer");
            } else if (Type != EWriterType::File && !FileName.empty()) {
                THROW_ERROR_EXCEPTION("Unused \"file_name\" attribute for %Qlv writer", Type);
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TWriterConfig)

////////////////////////////////////////////////////////////////////////////////

class TRuleConfig
    : public NYTree::TYsonSerializable
{
public:
    TNullable<yhash_set<TString>> IncludeCategories;
    yhash_set<TString> ExcludeCategories;

    ELogLevel MinLevel;
    ELogLevel MaxLevel;

    std::vector<TString> Writers;

    TRuleConfig()
    {
        RegisterParameter("include_categories", IncludeCategories)
            .Default();
        RegisterParameter("exclude_categories", ExcludeCategories)
            .Default();
        RegisterParameter("min_level", MinLevel)
            .Default(ELogLevel::Minimum);
        RegisterParameter("max_level", MaxLevel)
            .Default(ELogLevel::Maximum);
        RegisterParameter("writers", Writers)
            .NonEmpty();
    }

    bool IsApplicable(const TString& category) const;
    bool IsApplicable(const TString& category, ELogLevel level) const;
};

DEFINE_REFCOUNTED_TYPE(TRuleConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogConfig
    : public NYTree::TYsonSerializable
{
public:
    TNullable<TDuration> FlushPeriod;
    TNullable<TDuration> WatchPeriod;
    TNullable<TDuration> CheckSpacePeriod;

    i64 MinDiskSpace;

    int HighBacklogWatermark;
    int LowBacklogWatermark;

    TDuration ShutdownGraceTimeout;

    std::vector<TRuleConfigPtr> Rules;
    yhash<TString, TWriterConfigPtr> WriterConfigs;
    std::vector<TString> SuppressedMessages;

    TLogConfig()
    {
        RegisterParameter("flush_period", FlushPeriod)
            .Default();
        RegisterParameter("watch_period", WatchPeriod)
            .Default();
        RegisterParameter("check_space_period", CheckSpacePeriod)
            .Default();
        RegisterParameter("min_disk_space", MinDiskSpace)
            .GreaterThanOrEqual(1_GB)
            .Default(5_GB);
        RegisterParameter("high_backlog_watermark", HighBacklogWatermark)
            .GreaterThan(0)
            .Default(10000000);
        RegisterParameter("low_backlog_watermark", LowBacklogWatermark)
            .GreaterThan(0)
            .Default(1000000);
        RegisterParameter("shutdown_grace_timeout", ShutdownGraceTimeout)
            .Default(TDuration::Seconds(1));

        RegisterParameter("writers", WriterConfigs);
        RegisterParameter("rules", Rules);
        RegisterParameter("suppressed_messages", SuppressedMessages)
            .Default();

        RegisterValidator([&] () {
            for (const auto& rule : Rules) {
                for (const TString& writer : rule->Writers) {
                    if (WriterConfigs.find(writer) == WriterConfigs.end()) {
                        THROW_ERROR_EXCEPTION("Unknown writer %Qv", writer);
                    }
                }
            }
        });
    }

    static TLogConfigPtr CreateStderrLogger(ELogLevel logLevel);
    static TLogConfigPtr CreateLogFile(const TString& path);
    static TLogConfigPtr CreateDefault();
    static TLogConfigPtr CreateQuiet();
    static TLogConfigPtr CreateSilent();
    static TLogConfigPtr CreateFromFile(const TString& file, const NYPath::TYPath& path = "");
    static TLogConfigPtr CreateFromNode(NYTree::INodePtr node, const NYPath::TYPath& path = "");
};

DEFINE_REFCOUNTED_TYPE(TLogConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
