#pragma once

#include "fwd.h"
#include "raw_transform.h"

#include <list>
#include <vector>

namespace NRoren::NPrivate {

////////////////////////////////////////////////////////////////////////////////

class IParDoTree
    : public IRawParDo
{
public:
    virtual std::vector<TDynamicTypeTag> GetOriginalOutputTags() const = 0;
    virtual TString GetDebugDescription() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TParDoTreeBuilder
{
public:
    using TPCollectionNodeId = int;
    static constexpr TPCollectionNodeId RootNodeId = 0;

public:
    std::vector<TPCollectionNodeId> AddParDo(IRawParDoPtr parDo, TPCollectionNodeId input);
    void MarkAsOutput(TPCollectionNodeId nodeId, const TDynamicTypeTag& typeTag = {});
    void MarkAsOutputs(const std::vector<TPCollectionNodeId>& nodeIds);
    IParDoTreePtr Build();

private:
    struct TParDoNode
    {
        IRawParDoPtr ParDo;
        TPCollectionNodeId Input;
        std::vector<TPCollectionNodeId> Outputs;

        void Save(IOutputStream* os) const;
        void Load(IInputStream* is);
    };

    struct TPCollectionNode
    {
        int GlobalOutputIndex = InvalidOutputIndex;
        TRowVtable RowVtable;
    };

private:
    TPCollectionNodeId AddPCollectionNode(const TRowVtable& sourceParDoNode);
    void CheckNoHangingPCollectionNodes() const;
    void CheckPCollectionType(int pCollectionNodeId, const TRowVtable& rowVtable);

private:
    static constexpr int InvalidOutputIndex = -1;

    std::vector<TPCollectionNode> PCollectionNodes_ = {{}};
    std::vector<TParDoNode> ParDoNodes_;

    // Type tags of outputs marked with 'MarkAsOutput' / 'MarkAsOutputs'
    std::vector<TDynamicTypeTag> MarkedOutputTypeTags_;

    bool Built_ = false;

    class TParDoTree;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoren::NPrivate
