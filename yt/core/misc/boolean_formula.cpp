#include "boolean_formula.h"
#include "phoenix.h"

#include <yt/core/misc/error.h>

#include <yt/core/yson/string.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node.h>

namespace NYT {

using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void ValidateBooleanFormulaVariable(const Stroka& variable)
{
    for (auto c : variable) {
        if (c == '|' || c == '&' || c == '!' || c == '(' || c == ')' || c == ' ') {
            THROW_ERROR_EXCEPTION("Invalid character %Qv in boolean formula variable %Qv", c, variable);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EBooleanFormulaTokenType,
    (Variable)
    (Or)
    (And)
    (Not)
    (LeftBracket)
    (RightBracket)
);

struct TBooleanFormulaToken
{
    EBooleanFormulaTokenType Type;
    int Position;
    Stroka Name;
};

bool operator==(const TBooleanFormulaToken& lhs, const TBooleanFormulaToken& rhs)
{
    return lhs.Type == rhs.Type && lhs.Name == rhs.Name;
}

bool operator!=(const TBooleanFormulaToken& lhs, const TBooleanFormulaToken& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

class TBooleanFormula::TImpl
{
public:
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Formula);
    DEFINE_BYVAL_RO_PROPERTY(size_t, Hash);

public:
    TImpl(const Stroka& formula, size_t hash, std::vector<TBooleanFormulaToken> parsedFormula);

    bool operator==(const TImpl& other) const;

    bool IsEmpty() const;

    int Size() const;

    bool IsSatisfiedBy(const std::vector<Stroka>& value) const;
    bool IsSatisfiedBy(const yhash_set<Stroka>& value) const;

private:
    std::vector<TBooleanFormulaToken> ParsedFormula_;

    static std::vector<TBooleanFormulaToken> Tokenize(const Stroka& formula);
    static std::vector<TBooleanFormulaToken> Parse(
        const Stroka& formula,
        const std::vector<TBooleanFormulaToken>& tokens);
    static size_t CalculateHash(const std::vector<TBooleanFormulaToken> tokens);

    friend std::unique_ptr<TImpl> MakeBooleanFormulaImpl(const Stroka& formula);
};

////////////////////////////////////////////////////////////////////////////////

TBooleanFormula::TImpl::TImpl(const Stroka& formula, size_t hash, std::vector<TBooleanFormulaToken> parsedFormula)
    : Formula_(formula)
    , Hash_(hash)
    , ParsedFormula_(std::move(parsedFormula))
{ }

bool TBooleanFormula::TImpl::operator==(const TImpl& other) const
{
    if (ParsedFormula_.size() != other.ParsedFormula_.size()) {
        return false;
    }

    for (int index = 0; index < ParsedFormula_.size(); ++index) {
        if (ParsedFormula_[index] != other.ParsedFormula_[index]) {
            return false;
        }
    }

    return true;
}

bool TBooleanFormula::TImpl::IsEmpty() const
{
    return ParsedFormula_.empty();
}

int TBooleanFormula::TImpl::Size() const
{
    return ParsedFormula_.size();
}

bool TBooleanFormula::TImpl::IsSatisfiedBy(const std::vector<Stroka>& value) const
{
    yhash_set<Stroka> set(value.begin(), value.end());
    return IsSatisfiedBy(set);
}

bool TBooleanFormula::TImpl::IsSatisfiedBy(const yhash_set<Stroka>& value) const
{
    std::vector<bool> stack;

    for (const auto& token : ParsedFormula_) {
        switch (token.Type) {
            case EBooleanFormulaTokenType::Variable:
                stack.push_back(value.find(token.Name) != value.end());
                break;

            case EBooleanFormulaTokenType::Or: {
                YCHECK(stack.size() >= 2);
                bool lhs = stack[stack.size() - 2];
                bool rhs = stack[stack.size() - 1];
                stack.pop_back();
                stack.pop_back();
                stack.push_back(lhs || rhs);
                break;
            }

            case EBooleanFormulaTokenType::And: {
                YCHECK(stack.size() >= 2);
                bool lhs = stack[stack.size() - 2];
                bool rhs = stack[stack.size() - 1];
                stack.pop_back();
                stack.pop_back();
                stack.push_back(lhs && rhs);
                break;
            }

            case EBooleanFormulaTokenType::Not:
                YCHECK(stack.size() >= 1);
                stack.back() = !stack.back();
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    YCHECK(stack.size() <= 1);
    return stack.empty() ? true : stack[0];
}

std::vector<TBooleanFormulaToken> TBooleanFormula::TImpl::Tokenize(const Stroka& formula)
{
    std::vector<TBooleanFormulaToken> result;
    int begin = 0;
    int end;

    auto extractVariable = [&] () {
        if (begin < end) {
            result.push_back(TBooleanFormulaToken{
                EBooleanFormulaTokenType::Variable,
                begin,
                Stroka(TStringBuf(formula).SubStr(begin, end - begin))});
        }
    };

    auto addToken = [&] (EBooleanFormulaTokenType type) {
        result.push_back(TBooleanFormulaToken{type, end});
    };
    
    for (end = 0; end < formula.Size(); ++end) {
        switch (formula[end]) {
            case '|':
                extractVariable();
                addToken(EBooleanFormulaTokenType::Or);
                begin = end + 1;
                break;

            case '&':
                extractVariable();
                addToken(EBooleanFormulaTokenType::And);
                begin = end + 1;
                break;

            case '!':
                extractVariable();
                addToken(EBooleanFormulaTokenType::Not);
                begin = end + 1;
                break;

            case '(':
                extractVariable();
                addToken(EBooleanFormulaTokenType::LeftBracket);
                begin = end + 1;
                break;

            case ')':
                extractVariable();
                addToken(EBooleanFormulaTokenType::RightBracket);
                begin = end + 1;
                break;

            case ' ':
                extractVariable();
                begin = end + 1;
                break;

            default:
                break;
        }
    }
    extractVariable();

    return result;
}

std::vector<TBooleanFormulaToken> TBooleanFormula::TImpl::Parse(
    const Stroka& formula,
    const std::vector<TBooleanFormulaToken>& tokens)
{
    std::vector<TBooleanFormulaToken> result;
    std::vector<TBooleanFormulaToken> stack;
    bool expectSubformula = true;

    auto finishSubformula = [&] () {
        while (!stack.empty() && stack.back().Type != EBooleanFormulaTokenType::LeftBracket) {
            result.push_back(stack.back());
            stack.pop_back();
        }
    };

    auto throwError = [&] (int position, const Stroka& message) {
        TStringBuilder builder;
        Format(&builder, "Error while parsing boolean formula:\n%v\n", formula);
        builder.AppendChar(' ', position);
        Format(&builder, "^\n%v", message);
        THROW_ERROR_EXCEPTION(builder.Flush());
    };

    for (const auto& token : tokens) {
        switch (token.Type) {
            case EBooleanFormulaTokenType::Variable:
                if (!expectSubformula) {
                    throwError(token.Position, "Unexpected variable");
                }
                result.push_back(token);
                finishSubformula();
                expectSubformula = false;
                break;

            case EBooleanFormulaTokenType::Or:
            case EBooleanFormulaTokenType::And:
                if (expectSubformula || (!stack.empty() && stack.back().Type != EBooleanFormulaTokenType::LeftBracket)) {
                    throwError(token.Position, "Unexpected token");
                }
                stack.push_back(token);
                expectSubformula = true;
                break;

            case EBooleanFormulaTokenType::Not:
            case EBooleanFormulaTokenType::LeftBracket:
                if (!expectSubformula) {
                    throwError(token.Position, "Unexpected token");
                }
                stack.push_back(token);
                break;

            case EBooleanFormulaTokenType::RightBracket:
                if (expectSubformula || stack.empty() || stack.back().Type != EBooleanFormulaTokenType::LeftBracket) {
                    throwError(token.Position, "Unexpected token");
                }
                stack.pop_back();
                finishSubformula();
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    if (!stack.empty()) {
        throwError(formula.Size(), "Unfinished formula");
    }

    return result;
}

size_t TBooleanFormula::TImpl::CalculateHash(const std::vector<TBooleanFormulaToken> tokens)
{
    const size_t multiplier = 1000003;
    size_t result = 10000005;

    for (const auto& token : tokens) {
        result = result * multiplier + static_cast<size_t>(token.Type) + token.Name.hash();
    }

    return result;
}

std::unique_ptr<TBooleanFormula::TImpl> MakeBooleanFormulaImpl(const Stroka& formula)
{
    auto tokens = TBooleanFormula::TImpl::Tokenize(formula);
    auto parsed = TBooleanFormula::TImpl::Parse(formula, tokens);
    auto hash = TBooleanFormula::TImpl::CalculateHash(parsed);
    return std::make_unique<TBooleanFormula::TImpl>(formula, hash, parsed);
}

////////////////////////////////////////////////////////////////////////////////

TBooleanFormula::TBooleanFormula()
    : Impl_(MakeBooleanFormulaImpl(Stroka()))
{ }

TBooleanFormula::TBooleanFormula(std::unique_ptr<TBooleanFormula::TImpl> impl)
    : Impl_(std::move(impl))
{ }

TBooleanFormula::TBooleanFormula(const TBooleanFormula& other) = default;
TBooleanFormula::TBooleanFormula(TBooleanFormula&& other) = default;
TBooleanFormula& TBooleanFormula::operator=(const TBooleanFormula& other) = default;
TBooleanFormula& TBooleanFormula::operator=(TBooleanFormula&& other) = default;
TBooleanFormula::~TBooleanFormula() = default;

bool TBooleanFormula::operator==(const TBooleanFormula& other) const
{
    return *Impl_ == *other.Impl_;
}

bool TBooleanFormula::IsEmpty() const
{
    return Impl_->IsEmpty();
}

int TBooleanFormula::Size() const
{
    return Impl_->Size();
}

size_t TBooleanFormula::GetHash() const
{
    return Impl_->GetHash();
}

Stroka TBooleanFormula::GetFormula() const
{
    return Impl_->GetFormula();
}

bool TBooleanFormula::IsSatisfiedBy(const std::vector<Stroka>& value) const
{
    return Impl_->IsSatisfiedBy(value);
}

bool TBooleanFormula::IsSatisfiedBy(const yhash_set<Stroka>& value) const
{
    return Impl_->IsSatisfiedBy(value);
}

TBooleanFormula MakeBooleanFormula(const Stroka& formula)
{
    auto impl = MakeBooleanFormulaImpl(formula);
    return TBooleanFormula(std::move(impl));
}

void Serialize(const TBooleanFormula& booleanFormula, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .Value(booleanFormula.GetFormula());
}

void Deserialize(TBooleanFormula& booleanFormula, NYTree::INodePtr node)
{
    booleanFormula = MakeBooleanFormula(node->AsString()->GetValue());
}

void TBooleanFormula::Save(TStreamSaveContext& context) const
{
    using NYT::Save;
    Save(context, GetFormula());
}

void TBooleanFormula::Load(TStreamLoadContext& context)
{
    using NYT::Load;
    auto formula = Load<Stroka>(context);
    Impl_ = MakeBooleanFormulaImpl(formula);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

