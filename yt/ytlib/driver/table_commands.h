#pragma once

#include "command.h"

#include <yt/ytlib/formats/format.h>

#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/ytlib/ypath/rich.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class TReadTableCommand
    : public TTypedCommand<NApi::TTableReaderOptions>
{
public:
    TReadTableCommand();

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableReader;
    NFormats::TControlAttributesConfigPtr ControlAttributes;
    bool Unordered;

    virtual void OnLoaded() override;
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TReadBlobTableCommand
    : public TTypedCommand<NApi::TTableReaderOptions>
{
public:
    TReadBlobTableCommand();

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableReader;

    TNullable<TString> PartIndexColumnName;
    TNullable<TString> DataColumnName;

    virtual void OnLoaded() override;
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TLocateSkynetShareCommand
    : public TTypedCommand<NApi::TLocateSkynetShareOptions>
{
public:
    TLocateSkynetShareCommand();

private:
    NYPath::TRichYPath Path;

    virtual void OnLoaded() override;
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TWriteTableCommand
    : public TTypedCommand<NApi::TTableWriterOptions>
{
public:
    TWriteTableCommand();

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableWriter;

    virtual void OnLoaded() override;
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

template <class TOptions>
class TTabletCommandBase
    : public TTypedCommand<TOptions>
{
protected:
    NYPath::TRichYPath Path;

    TTabletCommandBase()
    {
        this->RegisterParameter("path", Path);
        this->RegisterParameter("first_tablet_index", this->Options.FirstTabletIndex)
            .Default();
        this->RegisterParameter("last_tablet_index", this->Options.LastTabletIndex)
            .Default();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TMountTableCommand
    : public TTabletCommandBase<NApi::TMountTableOptions>
{
public:
    TMountTableCommand();

private:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TUnmountTableCommand
    : public TTabletCommandBase<NApi::TUnmountTableOptions>
{
public:
    TUnmountTableCommand();

private:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TRemountTableCommand
    : public TTabletCommandBase<NApi::TRemountTableOptions>
{
public:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TFreezeTableCommand
    : public TTabletCommandBase<NApi::TFreezeTableOptions>
{
private:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TUnfreezeTableCommand
    : public TTabletCommandBase<NApi::TUnfreezeTableOptions>
{
public:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TReshardTableCommand
    : public TTabletCommandBase<NApi::TReshardTableOptions>
{
public:
    TReshardTableCommand();

private:
    TNullable<std::vector<NTableClient::TOwningKey>> PivotKeys;
    TNullable<int> TabletCount;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TAlterTableCommand
    : public TTypedCommand<NApi::TAlterTableOptions>
{
public: 
    TAlterTableCommand();

private:
    NYPath::TRichYPath Path;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TSelectRowsOptions
    : public NApi::TSelectRowsOptions
    , public TTabletReadOptions
{ };

class TSelectRowsCommand
    : public TTypedCommand<TSelectRowsOptions>
{
public:
    TSelectRowsCommand();

private:
    TString Query;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TInsertRowsCommand
    : public TTypedCommand<TInsertRowsOptions>
{
public:
    TInsertRowsCommand();

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;
    bool Update;
    bool Aggregate;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TLookupRowsOptions
    : public NApi::TLookupRowsOptions
    , public TTabletReadOptions
{ };

class TLookupRowsCommand
    : public TTypedCommand<TLookupRowsOptions>
{
public:
    TLookupRowsCommand();

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;
    TNullable<std::vector<TString>> ColumnNames;
    bool Versioned;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TGetInSyncReplicasCommand
    : public TTypedCommand<NApi::TGetInSyncReplicasOptions>
{
public:
    TGetInSyncReplicasCommand();

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TDeleteRowsCommand
    : public TTypedCommand<TDeleteRowsOptions>
{
public:
    TDeleteRowsCommand();

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TTrimRowsCommand
    : public TTypedCommand<NApi::TTrimTableOptions>
{
public:
    TTrimRowsCommand();

private:
    NYPath::TRichYPath Path;
    int TabletIndex;
    i64 TrimmedRowCount;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TEnableTableReplicaCommand
    : public TTypedCommand<NApi::TEnableTableReplicaOptions>
{
public:
    TEnableTableReplicaCommand();

private:
    NTabletClient::TTableReplicaId ReplicaId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TDisableTableReplicaCommand
    : public TTypedCommand<NApi::TDisableTableReplicaOptions>
{
public:
    TDisableTableReplicaCommand();

private:
    NTabletClient::TTableReplicaId ReplicaId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TAlterTableReplicaCommand
    : public TTypedCommand<NApi::TAlterTableReplicaOptions>
{
public:
    TAlterTableReplicaCommand();

private:
    NTabletClient::TTableReplicaId ReplicaId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
