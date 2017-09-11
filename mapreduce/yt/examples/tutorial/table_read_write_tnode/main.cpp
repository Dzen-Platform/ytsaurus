#include <mapreduce/yt/interface/client.h>

#include <util/system/user.h>

using namespace NYT;

int main(int argc, const char** argv) {
    NYT::Initialize(argc, argv);

    auto client = CreateClient("freud");

    const TString table = "//tmp/" + GetUsername() + "-read-write";

    {
        // Просто пишем данные в таблицу, если таблица существует, её перезапишут.
        auto writer = client->CreateTableWriter<TNode>(table);

        TNode row;
        row["english"] = "one";
        row["russian"] = "один";
        writer->AddRow(row);

        row["english"] = "two";
        row["russian"] = "два";
        writer->AddRow(row);

        // Лучше не забывать звать метод Finish явно.
        // Он конечно позовётся в деструкторе writer'а, но если случатся какие-нибудь ошибки,
        // то деструктор их молчаливо проглотит.
        writer->Finish();
    }
    {
        // Дописываем данные в конец таблицы,
        // Для этого необходимо выставить опцию Append.
        auto writer = client->CreateTableWriter<TNode>(TRichYPath(table).Append(true));

        TNode row;
        row["english"] = "three";
        row["russian"] = "три";
        writer->AddRow(row);

        writer->Finish();
    }
    {
        // Читаем всю таблицу
        auto reader = client->CreateTableReader<TNode>(table);
        Cout << "*** ALL TABLE ***" << Endl;
        for (; reader->IsValid(); reader->Next()) { // reader имеет тот же самый интерфейс, что и reader в джобах
            auto& row = reader->GetRow();
            Cout << "russian: " << row["russian"].AsString() << "; " << "english: " << row["english"].AsString() << Endl;
        }
        Cout << Endl;
    }
    {
        // Читаем первые 2 строки.
        auto reader = client->CreateTableReader<TNode>(TRichYPath(table).AddRange(
                TReadRange()
                    .LowerLimit(TReadLimit().RowIndex(0))
                    .UpperLimit(TReadLimit().RowIndex(2)))); // читаем с 0й по 2ю строки, 2я невключительно

        Cout << "*** FIRST TWO ROWS ***" << Endl;
        for (; reader->IsValid(); reader->Next()) { // reader имеет тот же самый интерфейс, что и reader в джобах
            auto& row = reader->GetRow();
            Cout << "russian: " << row["russian"].AsString() << "; " << "english: " << row["english"].AsString() << Endl;
        }
        Cout << Endl;
    }
    {
        // Мы можем отсортировать таблицу и читать записи по ключам.
        client->Sort(TSortOperationSpec()
            .SortBy({"english"})
            .AddInput(table)
            .Output(table));

        // Читаем первые 2 строки.
        auto reader = client->CreateTableReader<TNode>(TRichYPath(table).AddRange(
                TReadRange()
                    .Exact(TReadLimit().Key({"three"})))); // если нужен один ключ а не диапазон, можно использовать
                                                           // Exact вместо LowerLimit / UpperLimit,
                                                           // Key указывает, что мы ищем запись по ключу и работает
                                                           // только для сортированных таблиц.

        Cout << "*** EXACT KEY ***" << Endl;
        for (; reader->IsValid(); reader->Next()) { // reader имеет тот же самый интерфейс, что и reader в джобах
            auto& row = reader->GetRow();
            Cout << "russian: " << row["russian"].AsString() << "; " << "english: " << row["english"].AsString() << Endl;
        }
        Cout << Endl;
    }

    return 0;
}
