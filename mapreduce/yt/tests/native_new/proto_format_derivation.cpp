#include "lib.h"

#include <mapreduce/yt/tests/native_new/row.pb.h>

#include <library/unittest/registar.h>

#include <mapreduce/yt/interface/client.h>
#include <mapreduce/yt/interface/errors.h>
#include <mapreduce/yt/common/config.h>

using ::google::protobuf::Message;

using namespace NYT;
using namespace NYT::NTesting;

class TProtoFormatDerivationFixture {
public:
    TProtoFormatDerivationFixture()
        : Client_(CreateTestClient())
        , UseClientProtobufOld_(TConfig::Get()->UseClientProtobuf)
    {
        // Fill some data.
        {
            auto writer = Client_->CreateTableWriter<TNode>("//testing/urls1");
            writer->AddRow(TNode()("Host", "http://www.example.com")("Path", "/")("HttpCode", 302));
            writer->AddRow(TNode()("Host", "http://www.example.com")("Path", "/index.php")("HttpCode", 200));
            writer->Finish();
        }
        {
            auto writer = Client_->CreateTableWriter<TNode>("//testing/urls2");
            writer->AddRow(TNode()("Host", "http://www.example.com")("Path", "/index.htm")("HttpCode", 404));
            writer->AddRow(TNode()("Host", "http://www.other-example.com")("Path", "/")("HttpCode", 200));
            writer->Finish();
        }
        {
            auto writer = Client_->CreateTableWriter<TNode>("//testing/empty");
            writer->Finish();
        }

        TConfig::Get()->UseClientProtobuf = false;
    }

    ~TProtoFormatDerivationFixture()
    {
        TConfig::Get()->UseClientProtobuf = UseClientProtobufOld_;
    }

    const IClientPtr& GetClient()
    {
        return Client_;
    }

private:
    IClientPtr Client_;
    bool UseClientProtobufOld_ = false;
};

template<class TRow>
const TRow& GetRow(TTableReader<TRow>* reader)
{
    return reader->GetRow();
}

template<class TRow>
const TRow& GetRow(TTableReader<Message>* reader)
{
    return reader->GetRow<TRow>();
}

template <class TInputRow, class TOutputRow>
class TMapper
    : public IMapper<TTableReader<TInputRow>, TTableWriter<TOutputRow>>
{
public:
    void Do(TTableReader<TInputRow>* reader, TTableWriter<TOutputRow>* writer)
    {
        for (; reader->IsValid(); reader->Next()) {
            TUrlRow out = GetRow<TUrlRow>(reader);
            out.SetHttpCode(-out.GetHttpCode());
            writer->AddRow(out);
        }
    }
};

template <class TInputRow, class TOutputRow>
class TReduceCombiner
    : public IReducer<TTableReader<TInputRow>, TTableWriter<TOutputRow>>
{
public:
    void Do(TTableReader<TInputRow>* reader, TTableWriter<TOutputRow>* writer)
    {
        const Stroka host = GetRow<TUrlRow>(reader).GetHost();
        ui64 httpCodeTotal = 0;
        for (; reader->IsValid(); reader->Next()) {
            const auto& row = GetRow<TUrlRow>(reader);
            httpCodeTotal += row.GetHttpCode();
        }
        TUrlRow urlRow;
        urlRow.SetHost(host);
        urlRow.SetHttpCode(httpCodeTotal);
        writer->AddRow(urlRow);
    }
};

template <class TInputRow, class TOutputRow>
class TReducer : public IReducer<TTableReader<TInputRow>, TTableWriter<TOutputRow>>
{
public:

    void Do(TTableReader<TInputRow>* reader, TTableWriter<TOutputRow>* writer)
    {
        const Stroka host = GetRow<TUrlRow>(reader).GetHost();
        ui64 httpCodeTotal = 0;
        for (; reader->IsValid(); reader->Next()) {
            const auto& row = GetRow<TUrlRow>(reader);
            httpCodeTotal += row.GetHttpCode();
        }
        THostRow hostRow;
        hostRow.SetHost(host);
        hostRow.SetHttpCodeTotal(httpCodeTotal);
        writer->AddRow(hostRow);
    }
};

using TUnspecifiedInputMapper = TMapper<Message, TUrlRow>;
REGISTER_MAPPER(TUnspecifiedInputMapper);
using TUnspecifiedOutputMapper = TMapper<TUrlRow, Message>;
REGISTER_MAPPER(TUnspecifiedOutputMapper);
using TEverythingSpecifiedMapper = TMapper<TUrlRow, TUrlRow>;
REGISTER_MAPPER(TEverythingSpecifiedMapper);

using TUnspecifiedInputReduceCombiner = TReduceCombiner<Message, TUrlRow>;
REGISTER_REDUCER(TUnspecifiedInputReduceCombiner);
using TUnspecifiedOutputReduceCombiner = TReduceCombiner<TUrlRow, Message>;
REGISTER_REDUCER(TUnspecifiedOutputReduceCombiner);
using TEverythingSpecifiedReduceCombiner = TReduceCombiner<TUrlRow, TUrlRow>;
REGISTER_REDUCER(TEverythingSpecifiedReduceCombiner);

using TUnspecifiedInputReducer = TReducer<Message, THostRow>;
REGISTER_REDUCER(TUnspecifiedInputReducer);
using TEverythingSpecifiedReducer = TReducer<TUrlRow, THostRow>;
REGISTER_REDUCER(TEverythingSpecifiedReducer);


SIMPLE_UNIT_TEST_SUITE(ProtoFormatDerivation) {
    SIMPLE_UNIT_TEST(DifferentTypesMapperInput)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
            .ReduceBy("Host")
            .AddInput<TUrlRow>("//testing/urls1")
            .AddInput<TUrlRow>("//testing/urls2")
            //the only way to add different types' table is to make it empty
            .AddInput<THostRow>("//testing/empty")
            .AddOutput<THostRow>("//testing/host"),
            new TUnspecifiedInputMapper,
            new TEverythingSpecifiedReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }

    SIMPLE_UNIT_TEST(DifferentTypesNoMapperInput)
    {
        TProtoFormatDerivationFixture fixture;

        try {
            fixture.GetClient()->MapReduce(
                TMapReduceOperationSpec()
                    .ReduceBy("Host")
                    .AddInput<TUrlRow>("//testing/urls1")
                    .AddInput<THostRow>("//testing/urls2")
                    .AddOutput<THostRow>("//testing/host"),
                nullptr,
                new TEverythingSpecifiedReducer,
                TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
            UNIT_FAIL("operation was expected to fail");
        } catch (const TApiUsageError&) {
        }
    }

    SIMPLE_UNIT_TEST(UnspecifiedMapperOutput)
    {
        TProtoFormatDerivationFixture fixture;

        try {
            fixture.GetClient()->MapReduce(
                TMapReduceOperationSpec()
                    .ReduceBy("Host")
                    .AddInput<TUrlRow>("//testing/urls1")
                    .AddInput<TUrlRow>("//testing/urls2")
                    .AddOutput<THostRow>("//testing/host"),
                new TUnspecifiedOutputMapper,
                new TUnspecifiedInputReducer,
                TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
            UNIT_FAIL("operation was expected to fail");
        } catch (const TApiUsageError&) {
        }
    }

    SIMPLE_UNIT_TEST(HintedMapperOutput)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
            .ReduceBy("Host")
            .HintMapOutput<TUrlRow>()
            .AddInput<TUrlRow>("//testing/urls1")
            .AddInput<TUrlRow>("//testing/urls2")
            .AddOutput<THostRow>("//testing/host"),
            new TUnspecifiedOutputMapper,
            new TEverythingSpecifiedReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }

    SIMPLE_UNIT_TEST(UnspecifiedInputReduceCombiner)
    {
        TProtoFormatDerivationFixture fixture;

        try {
            fixture.GetClient()->MapReduce(
                TMapReduceOperationSpec()
                .ReduceBy("Host")
                .AddInput<TUrlRow>("//testing/urls1")
                .AddInput<TUrlRow>("//testing/urls2")
                .AddOutput<THostRow>("//testing/host"),
                new TEverythingSpecifiedMapper,
                new TUnspecifiedInputReduceCombiner,
                new TEverythingSpecifiedReducer,
                TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));

            UNIT_FAIL("operation was expected to fail");
        } catch (const TApiUsageError&) {
        }
    }

    SIMPLE_UNIT_TEST(HintedInputReduceCombiner)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
            .ReduceBy("Host")
            .AddInput<TUrlRow>("//testing/urls1")
            .AddInput<TUrlRow>("//testing/urls2")
            .AddOutput<THostRow>("//testing/host")
            .HintReduceCombinerInput<TUrlRow>(),
            new TEverythingSpecifiedMapper,
            new TUnspecifiedInputReduceCombiner,
            new TEverythingSpecifiedReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }

    SIMPLE_UNIT_TEST(UnspecifiedOutputReduceCombiner)
    {
        TProtoFormatDerivationFixture fixture;

        try {
            fixture.GetClient()->MapReduce(
                TMapReduceOperationSpec()
                .ReduceBy("Host")
                .AddInput<TUrlRow>("//testing/urls1")
                .AddInput<TUrlRow>("//testing/urls2")
                .AddOutput<THostRow>("//testing/host"),
                new TEverythingSpecifiedMapper,
                new TUnspecifiedOutputReduceCombiner,
                new TEverythingSpecifiedReducer,
                TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));

            UNIT_FAIL("operation was expected to fail");
        } catch (const TApiUsageError&) {
        }
    }

    SIMPLE_UNIT_TEST(HintedOutputReduceCombiner)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
            .ReduceBy("Host")
            .AddInput<TUrlRow>("//testing/urls1")
            .AddInput<TUrlRow>("//testing/urls2")
            .AddOutput<THostRow>("//testing/host")
            .HintReduceCombinerOutput<TUrlRow>(),
            new TEverythingSpecifiedMapper,
            new TUnspecifiedOutputReduceCombiner,
            new TEverythingSpecifiedReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }

    SIMPLE_UNIT_TEST(UnspecifiedReducerInput)
    {
        TProtoFormatDerivationFixture fixture;

        try {
            fixture.GetClient()->MapReduce(
                TMapReduceOperationSpec()
                    .ReduceBy("Host")
                    .AddInput<TNode>("//testing/urls1")
                    .AddInput<TNode>("//testing/urls2")
                    .AddOutput<THostRow>("//testing/host"),
                nullptr,
                new TUnspecifiedInputReducer,
                TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));

            UNIT_FAIL("operation was expected to fail");
        } catch (const TApiUsageError&) {
        }
    }

    SIMPLE_UNIT_TEST(ReducerInputFromOperationInput)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
                .ReduceBy("Host")
                .AddInput<TUrlRow>("//testing/urls1")
                .AddInput<TUrlRow>("//testing/urls2")
                .AddOutput<THostRow>("//testing/host"),
            nullptr,
            new TUnspecifiedInputReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }

    SIMPLE_UNIT_TEST(EverythingSpecified)
    {
        TProtoFormatDerivationFixture fixture;

        fixture.GetClient()->MapReduce(
            TMapReduceOperationSpec()
            .ReduceBy("Host")
            .AddInput<TUrlRow>("//testing/urls1")
            .AddInput<TUrlRow>("//testing/urls2")
            .AddOutput<THostRow>("//testing/host"),
            new TEverythingSpecifiedMapper,
            new TEverythingSpecifiedReduceCombiner,
            new TEverythingSpecifiedReducer,
            TOperationOptions().Spec(TNode()("max_failed_job_count", 1)));
    }
}
