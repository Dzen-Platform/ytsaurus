#pragma once

#include "public.h"

#include <yt/yt/core/ytree/public.h>
#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NLogging {

////////////////////////////////////////////////////////////////////////////////

class TWriterConfig
    : public NYTree::TYsonSerializable
{
public:
    EWriterType Type;
    TString FileName;
    ELogMessageFormat AcceptedMessageFormat;
    std::optional<size_t> RateLimit;
    bool EnableCompression;
    ECompressionMethod CompressionMethod;
    int CompressionLevel;
    THashMap<TString, NYTree::INodePtr> CommonFields;
    bool EnableSystemMessages;
    bool EnableSourceLocation;

    TWriterConfig()
    {
        RegisterParameter("type", Type);
        RegisterParameter("file_name", FileName)
            .Default();
        RegisterParameter("accepted_message_format", AcceptedMessageFormat)
            .Default(ELogMessageFormat::PlainText);
        RegisterParameter("rate_limit", RateLimit)
            .Default();
        RegisterParameter("enable_compression", EnableCompression)
            .Default(false);
        RegisterParameter("compression_method", CompressionMethod)
            .Default(ECompressionMethod::Gzip);
        RegisterParameter("compression_level", CompressionLevel)
            .Default(6);
        RegisterParameter("common_fields", CommonFields)
            .Default();
        RegisterParameter("enable_system_messages", EnableSystemMessages)
            .Alias("enable_control_messages")
            .Default(true);
        RegisterParameter("enable_source_location", EnableSourceLocation)
            .Default(false);

        RegisterPostprocessor([&] () {
            if (Type == EWriterType::File && FileName.empty()) {
                THROW_ERROR_EXCEPTION("Missing \"file_name\" attribute for \"file\" writer");
            } else if (Type != EWriterType::File && !FileName.empty()) {
                THROW_ERROR_EXCEPTION("Unused \"file_name\" attribute for %Qlv writer", Type);
            }

            if (CompressionMethod == ECompressionMethod::Gzip && (CompressionLevel < 0 || CompressionLevel > 9)) {
                THROW_ERROR_EXCEPTION("Invalid \"compression_level\" attribute for \"gzip\" compression method");
            } else if (CompressionMethod == ECompressionMethod::Zstd && CompressionLevel > 22) {
                THROW_ERROR_EXCEPTION("Invalid \"compression_level\" attribute for \"zstd\" compression method");
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
    std::optional<THashSet<TString>> IncludeCategories;
    THashSet<TString> ExcludeCategories;

    ELogLevel MinLevel;
    ELogLevel MaxLevel;

    ELogMessageFormat MessageFormat;

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
        RegisterParameter("message_format", MessageFormat)
            .Default(ELogMessageFormat::PlainText);
        RegisterParameter("writers", Writers)
            .NonEmpty();
    }

    bool IsApplicable(TStringBuf category, ELogMessageFormat format) const;
    bool IsApplicable(TStringBuf category, ELogLevel level, ELogMessageFormat format) const;
};

DEFINE_REFCOUNTED_TYPE(TRuleConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    std::optional<TDuration> FlushPeriod;
    std::optional<TDuration> WatchPeriod;
    std::optional<TDuration> CheckSpacePeriod;

    i64 MinDiskSpace;

    int HighBacklogWatermark;
    int LowBacklogWatermark;

    TDuration ShutdownGraceTimeout;

    std::vector<TRuleConfigPtr> Rules;
    THashMap<TString, TWriterConfigPtr> WriterConfigs;
    std::vector<TString> SuppressedMessages;
    THashMap<TString, size_t> CategoryRateLimits;

    TDuration RequestSuppressionTimeout;

    bool AbortOnAlert;

    TLogManagerConfig()
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
            .Default(10'000'000);
        RegisterParameter("low_backlog_watermark", LowBacklogWatermark)
            .GreaterThan(0)
            .Default(1'000'000);
        RegisterParameter("shutdown_grace_timeout", ShutdownGraceTimeout)
            .Default(TDuration::Seconds(1));

        RegisterParameter("writers", WriterConfigs);
        RegisterParameter("rules", Rules);
        RegisterParameter("suppressed_messages", SuppressedMessages)
            .Default();
        RegisterParameter("category_rate_limits", CategoryRateLimits)
            .Default();

        RegisterParameter("request_suppression_timeout", RequestSuppressionTimeout)
            .Alias("trace_suppression_timeout")
            .Default(TDuration::Zero());

        RegisterParameter("abort_on_alert", AbortOnAlert)
            .Default(false);

        RegisterPostprocessor([&] () {
            for (const auto& rule : Rules) {
                for (const TString& writer : rule->Writers) {
                    auto it = WriterConfigs.find(writer);
                    if (it == WriterConfigs.end()) {
                        THROW_ERROR_EXCEPTION("Unknown writer %Qv", writer);
                    }
                    if (rule->MessageFormat != it->second->AcceptedMessageFormat) {
                        THROW_ERROR_EXCEPTION("Writer %Qv does not accept message format %Qv",
                            writer,
                            rule->MessageFormat);
                    }
                }
            }
        });
    }

    static TLogManagerConfigPtr CreateStderrLogger(ELogLevel logLevel);
    static TLogManagerConfigPtr CreateLogFile(const TString& path);
    static TLogManagerConfigPtr CreateDefault();
    static TLogManagerConfigPtr CreateQuiet();
    static TLogManagerConfigPtr CreateSilent();
    //! Create logging config a-la YT server config: ./#componentName{,.debug,.error}.log.
    static TLogManagerConfigPtr CreateYtServer(const TString& componentName);
    static TLogManagerConfigPtr CreateFromFile(const TString& file, const NYPath::TYPath& path = "");
    static TLogManagerConfigPtr CreateFromNode(NYTree::INodePtr node, const NYPath::TYPath& path = "");
    static TLogManagerConfigPtr TryCreateFromEnv();
};

DEFINE_REFCOUNTED_TYPE(TLogManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogging
