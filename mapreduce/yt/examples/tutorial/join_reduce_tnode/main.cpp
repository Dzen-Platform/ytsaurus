#include <mapreduce/yt/interface/client.h>

#include <library/regex/pcre/regexp.h>

#include <util/system/user.h>

using namespace NYT;

class TFilterVideoRegexp
    : public IReducer<TTableReader<TNode>, TTableWriter<TNode>>
{
public:
    void Do(TReader* reader, TWriter* writer)
    {
        // Так же как и с обычным Reducer'ом в каждый вызов метода Do
        // придут записи с общим JoinBy ключом.
        TMaybe<TRegExMatch> regex;
        for (auto& cursor : *reader) {
            auto row = cursor.GetRow();
            if (cursor.GetTableIndex() == 0) { // таблица с хостами
                const auto videoRegexp = row["video_regexp"].AsString();

                // Дебажная печать, stderr можно будет посмотреть в web интерфейсе
                Cerr << "Processing host: " << row["host"].AsString() << Endl;
                if (!videoRegexp.empty()) {
                    regex = TRegExMatch(videoRegexp);
                }
            } else { // таблица с урлами
                if (regex && regex->Match(row["path"].AsString().c_str())) {
                    writer->AddRow(row);
                }
            }
        }
    }
};
REGISTER_REDUCER(TFilterVideoRegexp);

int main(int argc, const char** argv) {
    NYT::Initialize(argc, argv);

    auto client = CreateClient("freud");

    const TString outputTable = "//tmp/" + GetUsername() + "-tutorial-join-reduce";

    client->JoinReduce(
        TJoinReduceOperationSpec()
        .JoinBy({"host"})
        .AddInput<TNode>(
            TRichYPath("//home/ermolovd/yt-tutorial/host_video_regexp")
            .Foreign(true)) // важно отметить хостовую таблицу как foreign
        .AddInput<TNode>("//home/ermolovd/yt-tutorial/doc_title")
        .AddOutput<TNode>(outputTable),
        new TFilterVideoRegexp);

    Cout << "Output table: https://yt.yandex-team.ru/freud/#page=navigation&offsetMode=row&path=" << outputTable << Endl;

    return 0;
}

