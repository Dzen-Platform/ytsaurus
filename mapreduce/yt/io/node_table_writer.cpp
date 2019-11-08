#include "node_table_writer.h"

#include "proxy_output.h"

#include <mapreduce/yt/common/node_visitor.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <library/yson/writer.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TNodeTableWriter::TNodeTableWriter(THolder<TProxyOutput> output, EYsonFormat format)
    : Output_(std::move(output))
{
    for (size_t i = 0; i < Output_->GetStreamCount(); ++i) {
        Writers_.push_back(
            new TYsonWriter(Output_->GetStream(i), format, YT_LIST_FRAGMENT));
    }
}

TNodeTableWriter::~TNodeTableWriter()
{ }

size_t TNodeTableWriter::GetTableCount() const
{
    return Output_->GetStreamCount();
}

void TNodeTableWriter::FinishTable(size_t tableIndex) {
    Output_->GetStream(tableIndex)->Finish();
}

void TNodeTableWriter::AddRow(const TNode& row, size_t tableIndex)
{
    if (row.HasAttributes()) {
        ythrow TIOException() << "Row cannot have attributes";
    }

    static const TNode emptyMap = TNode::CreateMap();
    const TNode* outRow = &emptyMap;
    if (row.GetType() != TNode::Undefined) {
        if (!row.IsMap()) {
            ythrow TIOException() << "Row should be a map node";
        } else {
            outRow = &row;
        }
    }

    auto* writer = Writers_[tableIndex].Get();
    writer->OnListItem();

    TNodeVisitor visitor(writer);
    visitor.Visit(*outRow);

    Output_->OnRowFinished(tableIndex);
}

void TNodeTableWriter::AddRow(TNode&& row, size_t tableIndex) {
    AddRow(row, tableIndex);
}

void TNodeTableWriter::Abort()
{
    Output_->Abort();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
