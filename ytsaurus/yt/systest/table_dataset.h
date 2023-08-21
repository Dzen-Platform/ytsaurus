#pragma once

#include <yt/systest/dataset.h>
#include <yt/systest/table.h>

namespace NYT::NTest {

class TTableDataset : public IDataset
{
public:
    TTableDataset(const TTable& table, IClientPtr client, const TString& path);

    virtual const TTable& table_schema() const;
    virtual std::unique_ptr<IDatasetIterator> NewIterator() const;

private:
    const TTable& Table_;
    IClientPtr Client_;
    const TString Path_;
};

}  // namespace NYT::NTest
