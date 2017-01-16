#pragma once

#include "command.h"

#include <yt/ytlib/ypath/rich.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TReadFileCommand
    : public TTypedCommand<NApi::TFileReaderOptions>
{
public:
    TReadFileCommand();

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr FileReader;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TWriteFileCommand
    : public TTypedCommand<NApi::TFileWriterOptions>
{
public:
    TWriteFileCommand();

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr FileWriter;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

