#include "path_resolver.h"

#include <yt/yt/ytlib/sequoia_client/resolve_node.record.h>
#include <yt/yt/ytlib/sequoia_client/table_descriptor.h>
#include <yt/yt/ytlib/sequoia_client/transaction.h>

#include <yt/yt/client/cypress_client/public.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/misc/error.h>

#include <yt/yt/core/ypath/tokenizer.h>

#include <yt/yt/core/ytree/helpers.h>

namespace NYT::NCypressProxy {

using namespace NConcurrency;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NSequoiaClient;
using namespace NYTree;
using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

class TPathResolver
{
public:
    TPathResolver(
        ISequoiaTransactionPtr transaction,
        TYPath path)
        : Transaction_(std::move(transaction))
        , Path_(std::move(path))
    { }

    TResolveResult Resolve()
    {
        static const auto SlashYPath = TYPath("/");
        Tokenizer_.Reset(Path_);

        for (int resolveDepth = 0; ; ++resolveDepth) {
            ValidateYPathResolutionDepth(Path_, resolveDepth);

            if (auto rewrite = MaybeRewriteRoot()) {
                Tokenizer_.Reset(*rewrite);
                continue;
            }

            struct TResolveAttempt
            {
                TYPath Prefix;
                TYPath Suffix;
            };
            constexpr int TypicalTokenCount = 16;
            TCompactVector<TResolveAttempt, TypicalTokenCount> resolveAttempts;

            TYPath currentPrefix = SlashYPath;
            currentPrefix.reserve(Path_.size());

            while (Tokenizer_.Skip(ETokenType::Slash)) {
                if (Tokenizer_.GetType() != ETokenType::Literal) {
                    break;
                }
                auto literal = Tokenizer_.GetLiteralValue();

                currentPrefix += SlashYPath;
                currentPrefix += std::move(literal);

                Tokenizer_.Advance();

                resolveAttempts.push_back(TResolveAttempt{
                    .Prefix = currentPrefix,
                    .Suffix = TYPath(Tokenizer_.GetInput()),
                });
            }

            std::vector<NRecords::TResolveNodeKey> prefixKeys;
            prefixKeys.reserve(resolveAttempts.size());
            for (const auto& resolveAttempt : resolveAttempts) {
                prefixKeys.push_back(NRecords::TResolveNodeKey{
                    .Path = resolveAttempt.Prefix,
                });
            }

            // TODO(gritukan, babenko): Add column filters to codegen library.
            const auto& schema = ITableDescriptor::Get(ESequoiaTable::ResolveNode)
                ->GetRecordDescriptor()
                ->GetSchema();
            NTableClient::TColumnFilter columnFilter({
                schema->GetColumnIndex("path"),
                schema->GetColumnIndex("node_id"),
            });
            auto lookupRsps = WaitFor(Transaction_->LookupRows(prefixKeys, columnFilter))
                .ValueOrThrow();
            YT_VERIFY(lookupRsps.size() == prefixKeys.size());

            bool scionFound = false;
            TSequoiaResolveResult result;
            for (int index = 0; index < std::ssize(lookupRsps); ++index) {
                if (const auto& rsp = lookupRsps[index]) {
                    auto nodeId = ConvertTo<TNodeId>(rsp->NodeId);
                    if (TypeFromId(nodeId) == EObjectType::Scion) {
                        scionFound = true;
                    }

                    if (scionFound) {
                        const auto& resolveAttempt = resolveAttempts[index];
                        YT_VERIFY(resolveAttempt.Prefix == rsp->Key.Path);

                        result = TSequoiaResolveResult{
                            .ResolvedPrefix = resolveAttempt.Prefix,
                            .ResolvedPrefixNodeId = nodeId,
                            .UnresolvedSuffix = resolveAttempt.Suffix,
                        };
                    }
                }
            }

            if (scionFound) {
                return result;
            }

            return TCypressResolveResult{};
        }
    }

private:
    const ISequoiaTransactionPtr Transaction_;

    const TYPath Path_;

    TTokenizer Tokenizer_;

    std::optional<TYPath> MaybeRewriteRoot()
    {
        YT_VERIFY(Tokenizer_.Skip(ETokenType::StartOfStream));
        switch (Tokenizer_.GetType()) {
            case ETokenType::EndOfStream:
                THROW_ERROR_EXCEPTION("YPath cannot be empty");

            case ETokenType::Slash: {
                Tokenizer_.Advance();
                return std::nullopt;
            }

            case ETokenType::Literal: {
                auto token = Tokenizer_.GetToken();
                if (!token.StartsWith(ObjectIdPathPrefix)) {
                    Tokenizer_.ThrowUnexpected();
                }

                THROW_ERROR_EXCEPTION("Object id syntax is not supported yet");
            }

            default:
                Tokenizer_.ThrowUnexpected();
        }

        YT_ABORT();
    }
};

////////////////////////////////////////////////////////////////////////////////

TResolveResult ResolvePath(
    ISequoiaTransactionPtr transaction,
    TYPath path)
{
    TPathResolver resolver(
        std::move(transaction),
        std::move(path));
    return resolver.Resolve();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressProxy
