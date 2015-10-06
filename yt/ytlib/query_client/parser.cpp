// A Bison parser, made by GNU Bison 3.0.2.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2013 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// Take the name prefix into account.
#define yylex   yt_ql_yylex

// First part of user declarations.


# ifndef YY_NULLPTR
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULLPTR nullptr
#  else
#   define YY_NULLPTR 0
#  endif
# endif

#include "parser.hpp"

// User implementation prologue.

// Unqualified %code blocks.

    #include <ytlib/query_client/lexer.h>

    #define yt_ql_yylex lexer.GetNextToken

    #ifndef YYLLOC_DEFAULT
    #define YYLLOC_DEFAULT(Current, Rhs, N) \
        do { \
            if (N) { \
                (Current).first = YYRHSLOC(Rhs, 1).first; \
                (Current).second = YYRHSLOC (Rhs, N).second; \
            } else { \
                (Current).first = (Current).second = YYRHSLOC(Rhs, 0).second; \
            } \
        } while (false)
    #endif



#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

# ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                               \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).begin  = YYRHSLOC (Rhs, 1).begin;                   \
          (Current).end    = YYRHSLOC (Rhs, N).end;                     \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).begin = (Current).end = YYRHSLOC (Rhs, 0).end;      \
        }                                                               \
    while (/*CONSTCOND*/ false)
# endif


// Suppress unused-variable warnings by "using" E.
#define YYUSE(E) ((void) (E))

// Enable debugging if requested.
#if YT_QL_YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << std::endl;                  \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yystack_print_ ();                \
  } while (false)

#else // !YT_QL_YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YYUSE(Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void>(0)
# define YY_STACK_PRINT()                static_cast<void>(0)

#endif // !YT_QL_YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyempty = true)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

namespace NYT { namespace NQueryClient { namespace NAst {

  /* Return YYSTR after stripping away unnecessary quotes and
     backslashes, so that it's suitable for yyerror.  The heuristic is
     that double-quoting is unnecessary unless the string contains an
     apostrophe, a comma, or backslash (other than backslash-backslash).
     YYSTR is taken from yytname.  */
  std::string
  TParser::yytnamerr_ (const char *yystr)
  {
    if (*yystr == '"')
      {
        std::string yyr = "";
        char const *yyp = yystr;

        for (;;)
          switch (*++yyp)
            {
            case '\'':
            case ',':
              goto do_not_strip_quotes;

            case '\\':
              if (*++yyp != '\\')
                goto do_not_strip_quotes;
              // Fall through.
            default:
              yyr += *yyp;
              break;

            case '"':
              return yyr;
            }
      do_not_strip_quotes: ;
      }

    return yystr;
  }


  /// Build a parser object.
  TParser::TParser (TLexer& lexer_yyarg, TAstHead* head_yyarg, const Stroka& source_yyarg)
    :
#if YT_QL_YYDEBUG
      yydebug_ (false),
      yycdebug_ (&std::cerr),
#endif
      lexer (lexer_yyarg),
      head (head_yyarg),
      source (source_yyarg)
  {}

  TParser::~TParser ()
  {}


  /*---------------.
  | Symbol types.  |
  `---------------*/

  inline
  TParser::syntax_error::syntax_error (const location_type& l, const std::string& m)
    : std::runtime_error (m)
    , location (l)
  {}

  // basic_symbol.
  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol ()
    : value ()
  {}

  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol (const basic_symbol& other)
    : Base (other)
    , value ()
    , location (other.location)
  {
      switch (other.type_get ())
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.copy< EBinaryOp > (other.value);
        break;

      case 86: // unary-op
        value.copy< EUnaryOp > (other.value);
        break;

      case 31: // "string literal"
        value.copy< Stroka > (other.value);
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.copy< TExpressionList > (other.value);
        break;

      case 69: // identifier-list
        value.copy< TIdentifierList > (other.value);
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.copy< TLiteralValueList > (other.value);
        break;

      case 93: // const-tuple-list
        value.copy< TLiteralValueTupleList > (other.value);
        break;

      case 89: // literal-value
      case 90: // const-value
        value.copy< TNullable<TLiteralValue> > (other.value);
        break;

      case 66: // order-expr-list
        value.copy< TOrderExpressionList > (other.value);
        break;

      case 87: // qualified-identifier
        value.copy< TReferenceExpressionPtr > (other.value);
        break;

      case 27: // "identifier"
        value.copy< TStringBuf > (other.value);
        break;

      case 58: // table-descriptor
        value.copy< TTableDescriptor > (other.value);
        break;

      case 61: // is-left
      case 67: // is-desc
        value.copy< bool > (other.value);
        break;

      case 30: // "double literal"
        value.copy< double > (other.value);
        break;

      case 28: // "int64 literal"
        value.copy< i64 > (other.value);
        break;

      case 29: // "uint64 literal"
        value.copy< ui64 > (other.value);
        break;

      default:
        break;
    }

  }


  template <typename Base>
  inline
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const semantic_type& v, const location_type& l)
    : Base (t)
    , value ()
    , location (l)
  {
    (void) v;
      switch (this->type_get ())
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.copy< EBinaryOp > (v);
        break;

      case 86: // unary-op
        value.copy< EUnaryOp > (v);
        break;

      case 31: // "string literal"
        value.copy< Stroka > (v);
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.copy< TExpressionList > (v);
        break;

      case 69: // identifier-list
        value.copy< TIdentifierList > (v);
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.copy< TLiteralValueList > (v);
        break;

      case 93: // const-tuple-list
        value.copy< TLiteralValueTupleList > (v);
        break;

      case 89: // literal-value
      case 90: // const-value
        value.copy< TNullable<TLiteralValue> > (v);
        break;

      case 66: // order-expr-list
        value.copy< TOrderExpressionList > (v);
        break;

      case 87: // qualified-identifier
        value.copy< TReferenceExpressionPtr > (v);
        break;

      case 27: // "identifier"
        value.copy< TStringBuf > (v);
        break;

      case 58: // table-descriptor
        value.copy< TTableDescriptor > (v);
        break;

      case 61: // is-left
      case 67: // is-desc
        value.copy< bool > (v);
        break;

      case 30: // "double literal"
        value.copy< double > (v);
        break;

      case 28: // "int64 literal"
        value.copy< i64 > (v);
        break;

      case 29: // "uint64 literal"
        value.copy< ui64 > (v);
        break;

      default:
        break;
    }
}


  // Implementation of basic_symbol constructor for each type.

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const location_type& l)
    : Base (t)
    , value ()
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const EBinaryOp v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const EUnaryOp v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const Stroka v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TExpressionList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TIdentifierList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TLiteralValueList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TLiteralValueTupleList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TNullable<TLiteralValue> v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TOrderExpressionList v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TReferenceExpressionPtr v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TStringBuf v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const TTableDescriptor v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const bool v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const double v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const i64 v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}

  template <typename Base>
  TParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, const ui64 v, const location_type& l)
    : Base (t)
    , value (v)
    , location (l)
  {}


  template <typename Base>
  inline
  TParser::basic_symbol<Base>::~basic_symbol ()
  {
    // User destructor.
    symbol_number_type yytype = this->type_get ();
    switch (yytype)
    {
   default:
      break;
    }

    // Type destructor.
    switch (yytype)
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.template destroy< EBinaryOp > ();
        break;

      case 86: // unary-op
        value.template destroy< EUnaryOp > ();
        break;

      case 31: // "string literal"
        value.template destroy< Stroka > ();
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.template destroy< TExpressionList > ();
        break;

      case 69: // identifier-list
        value.template destroy< TIdentifierList > ();
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.template destroy< TLiteralValueList > ();
        break;

      case 93: // const-tuple-list
        value.template destroy< TLiteralValueTupleList > ();
        break;

      case 89: // literal-value
      case 90: // const-value
        value.template destroy< TNullable<TLiteralValue> > ();
        break;

      case 66: // order-expr-list
        value.template destroy< TOrderExpressionList > ();
        break;

      case 87: // qualified-identifier
        value.template destroy< TReferenceExpressionPtr > ();
        break;

      case 27: // "identifier"
        value.template destroy< TStringBuf > ();
        break;

      case 58: // table-descriptor
        value.template destroy< TTableDescriptor > ();
        break;

      case 61: // is-left
      case 67: // is-desc
        value.template destroy< bool > ();
        break;

      case 30: // "double literal"
        value.template destroy< double > ();
        break;

      case 28: // "int64 literal"
        value.template destroy< i64 > ();
        break;

      case 29: // "uint64 literal"
        value.template destroy< ui64 > ();
        break;

      default:
        break;
    }

  }

  template <typename Base>
  inline
  void
  TParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move(s);
      switch (this->type_get ())
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.move< EBinaryOp > (s.value);
        break;

      case 86: // unary-op
        value.move< EUnaryOp > (s.value);
        break;

      case 31: // "string literal"
        value.move< Stroka > (s.value);
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.move< TExpressionList > (s.value);
        break;

      case 69: // identifier-list
        value.move< TIdentifierList > (s.value);
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.move< TLiteralValueList > (s.value);
        break;

      case 93: // const-tuple-list
        value.move< TLiteralValueTupleList > (s.value);
        break;

      case 89: // literal-value
      case 90: // const-value
        value.move< TNullable<TLiteralValue> > (s.value);
        break;

      case 66: // order-expr-list
        value.move< TOrderExpressionList > (s.value);
        break;

      case 87: // qualified-identifier
        value.move< TReferenceExpressionPtr > (s.value);
        break;

      case 27: // "identifier"
        value.move< TStringBuf > (s.value);
        break;

      case 58: // table-descriptor
        value.move< TTableDescriptor > (s.value);
        break;

      case 61: // is-left
      case 67: // is-desc
        value.move< bool > (s.value);
        break;

      case 30: // "double literal"
        value.move< double > (s.value);
        break;

      case 28: // "int64 literal"
        value.move< i64 > (s.value);
        break;

      case 29: // "uint64 literal"
        value.move< ui64 > (s.value);
        break;

      default:
        break;
    }

    location = s.location;
  }

  // by_type.
  inline
  TParser::by_type::by_type ()
     : type (empty)
  {}

  inline
  TParser::by_type::by_type (const by_type& other)
    : type (other.type)
  {}

  inline
  TParser::by_type::by_type (token_type t)
    : type (yytranslate_ (t))
  {}

  inline
  void
  TParser::by_type::move (by_type& that)
  {
    type = that.type;
    that.type = empty;
  }

  inline
  int
  TParser::by_type::type_get () const
  {
    return type;
  }
  // Implementation of make_symbol for each symbol type.
  TParser::symbol_type
  TParser::make_End (const location_type& l)
  {
    return symbol_type (token::End, l);
  }

  TParser::symbol_type
  TParser::make_Failure (const location_type& l)
  {
    return symbol_type (token::Failure, l);
  }

  TParser::symbol_type
  TParser::make_StrayWillParseQuery (const location_type& l)
  {
    return symbol_type (token::StrayWillParseQuery, l);
  }

  TParser::symbol_type
  TParser::make_StrayWillParseJobQuery (const location_type& l)
  {
    return symbol_type (token::StrayWillParseJobQuery, l);
  }

  TParser::symbol_type
  TParser::make_StrayWillParseExpression (const location_type& l)
  {
    return symbol_type (token::StrayWillParseExpression, l);
  }

  TParser::symbol_type
  TParser::make_KwFrom (const location_type& l)
  {
    return symbol_type (token::KwFrom, l);
  }

  TParser::symbol_type
  TParser::make_KwWhere (const location_type& l)
  {
    return symbol_type (token::KwWhere, l);
  }

  TParser::symbol_type
  TParser::make_KwHaving (const location_type& l)
  {
    return symbol_type (token::KwHaving, l);
  }

  TParser::symbol_type
  TParser::make_KwLimit (const location_type& l)
  {
    return symbol_type (token::KwLimit, l);
  }

  TParser::symbol_type
  TParser::make_KwJoin (const location_type& l)
  {
    return symbol_type (token::KwJoin, l);
  }

  TParser::symbol_type
  TParser::make_KwUsing (const location_type& l)
  {
    return symbol_type (token::KwUsing, l);
  }

  TParser::symbol_type
  TParser::make_KwGroupBy (const location_type& l)
  {
    return symbol_type (token::KwGroupBy, l);
  }

  TParser::symbol_type
  TParser::make_KwOrderBy (const location_type& l)
  {
    return symbol_type (token::KwOrderBy, l);
  }

  TParser::symbol_type
  TParser::make_KwAsc (const location_type& l)
  {
    return symbol_type (token::KwAsc, l);
  }

  TParser::symbol_type
  TParser::make_KwDesc (const location_type& l)
  {
    return symbol_type (token::KwDesc, l);
  }

  TParser::symbol_type
  TParser::make_KwLeft (const location_type& l)
  {
    return symbol_type (token::KwLeft, l);
  }

  TParser::symbol_type
  TParser::make_KwAs (const location_type& l)
  {
    return symbol_type (token::KwAs, l);
  }

  TParser::symbol_type
  TParser::make_KwOn (const location_type& l)
  {
    return symbol_type (token::KwOn, l);
  }

  TParser::symbol_type
  TParser::make_KwAnd (const location_type& l)
  {
    return symbol_type (token::KwAnd, l);
  }

  TParser::symbol_type
  TParser::make_KwOr (const location_type& l)
  {
    return symbol_type (token::KwOr, l);
  }

  TParser::symbol_type
  TParser::make_KwNot (const location_type& l)
  {
    return symbol_type (token::KwNot, l);
  }

  TParser::symbol_type
  TParser::make_KwBetween (const location_type& l)
  {
    return symbol_type (token::KwBetween, l);
  }

  TParser::symbol_type
  TParser::make_KwIn (const location_type& l)
  {
    return symbol_type (token::KwIn, l);
  }

  TParser::symbol_type
  TParser::make_KwFalse (const location_type& l)
  {
    return symbol_type (token::KwFalse, l);
  }

  TParser::symbol_type
  TParser::make_KwTrue (const location_type& l)
  {
    return symbol_type (token::KwTrue, l);
  }

  TParser::symbol_type
  TParser::make_Identifier (const TStringBuf& v, const location_type& l)
  {
    return symbol_type (token::Identifier, v, l);
  }

  TParser::symbol_type
  TParser::make_Int64Literal (const i64& v, const location_type& l)
  {
    return symbol_type (token::Int64Literal, v, l);
  }

  TParser::symbol_type
  TParser::make_Uint64Literal (const ui64& v, const location_type& l)
  {
    return symbol_type (token::Uint64Literal, v, l);
  }

  TParser::symbol_type
  TParser::make_DoubleLiteral (const double& v, const location_type& l)
  {
    return symbol_type (token::DoubleLiteral, v, l);
  }

  TParser::symbol_type
  TParser::make_StringLiteral (const Stroka& v, const location_type& l)
  {
    return symbol_type (token::StringLiteral, v, l);
  }

  TParser::symbol_type
  TParser::make_OpTilde (const location_type& l)
  {
    return symbol_type (token::OpTilde, l);
  }

  TParser::symbol_type
  TParser::make_OpVerticalBar (const location_type& l)
  {
    return symbol_type (token::OpVerticalBar, l);
  }

  TParser::symbol_type
  TParser::make_OpAmpersand (const location_type& l)
  {
    return symbol_type (token::OpAmpersand, l);
  }

  TParser::symbol_type
  TParser::make_OpModulo (const location_type& l)
  {
    return symbol_type (token::OpModulo, l);
  }

  TParser::symbol_type
  TParser::make_OpLeftShift (const location_type& l)
  {
    return symbol_type (token::OpLeftShift, l);
  }

  TParser::symbol_type
  TParser::make_OpRightShift (const location_type& l)
  {
    return symbol_type (token::OpRightShift, l);
  }

  TParser::symbol_type
  TParser::make_LeftParenthesis (const location_type& l)
  {
    return symbol_type (token::LeftParenthesis, l);
  }

  TParser::symbol_type
  TParser::make_RightParenthesis (const location_type& l)
  {
    return symbol_type (token::RightParenthesis, l);
  }

  TParser::symbol_type
  TParser::make_Asterisk (const location_type& l)
  {
    return symbol_type (token::Asterisk, l);
  }

  TParser::symbol_type
  TParser::make_OpPlus (const location_type& l)
  {
    return symbol_type (token::OpPlus, l);
  }

  TParser::symbol_type
  TParser::make_Comma (const location_type& l)
  {
    return symbol_type (token::Comma, l);
  }

  TParser::symbol_type
  TParser::make_OpMinus (const location_type& l)
  {
    return symbol_type (token::OpMinus, l);
  }

  TParser::symbol_type
  TParser::make_Dot (const location_type& l)
  {
    return symbol_type (token::Dot, l);
  }

  TParser::symbol_type
  TParser::make_OpDivide (const location_type& l)
  {
    return symbol_type (token::OpDivide, l);
  }

  TParser::symbol_type
  TParser::make_OpLess (const location_type& l)
  {
    return symbol_type (token::OpLess, l);
  }

  TParser::symbol_type
  TParser::make_OpLessOrEqual (const location_type& l)
  {
    return symbol_type (token::OpLessOrEqual, l);
  }

  TParser::symbol_type
  TParser::make_OpEqual (const location_type& l)
  {
    return symbol_type (token::OpEqual, l);
  }

  TParser::symbol_type
  TParser::make_OpNotEqual (const location_type& l)
  {
    return symbol_type (token::OpNotEqual, l);
  }

  TParser::symbol_type
  TParser::make_OpGreater (const location_type& l)
  {
    return symbol_type (token::OpGreater, l);
  }

  TParser::symbol_type
  TParser::make_OpGreaterOrEqual (const location_type& l)
  {
    return symbol_type (token::OpGreaterOrEqual, l);
  }



  // by_state.
  inline
  TParser::by_state::by_state ()
    : state (empty)
  {}

  inline
  TParser::by_state::by_state (const by_state& other)
    : state (other.state)
  {}

  inline
  void
  TParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.state = empty;
  }

  inline
  TParser::by_state::by_state (state_type s)
    : state (s)
  {}

  inline
  TParser::symbol_number_type
  TParser::by_state::type_get () const
  {
    return state == empty ? 0 : yystos_[state];
  }

  inline
  TParser::stack_symbol_type::stack_symbol_type ()
  {}


  inline
  TParser::stack_symbol_type::stack_symbol_type (state_type s, symbol_type& that)
    : super_type (s, that.location)
  {
      switch (that.type_get ())
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.move< EBinaryOp > (that.value);
        break;

      case 86: // unary-op
        value.move< EUnaryOp > (that.value);
        break;

      case 31: // "string literal"
        value.move< Stroka > (that.value);
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.move< TExpressionList > (that.value);
        break;

      case 69: // identifier-list
        value.move< TIdentifierList > (that.value);
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.move< TLiteralValueList > (that.value);
        break;

      case 93: // const-tuple-list
        value.move< TLiteralValueTupleList > (that.value);
        break;

      case 89: // literal-value
      case 90: // const-value
        value.move< TNullable<TLiteralValue> > (that.value);
        break;

      case 66: // order-expr-list
        value.move< TOrderExpressionList > (that.value);
        break;

      case 87: // qualified-identifier
        value.move< TReferenceExpressionPtr > (that.value);
        break;

      case 27: // "identifier"
        value.move< TStringBuf > (that.value);
        break;

      case 58: // table-descriptor
        value.move< TTableDescriptor > (that.value);
        break;

      case 61: // is-left
      case 67: // is-desc
        value.move< bool > (that.value);
        break;

      case 30: // "double literal"
        value.move< double > (that.value);
        break;

      case 28: // "int64 literal"
        value.move< i64 > (that.value);
        break;

      case 29: // "uint64 literal"
        value.move< ui64 > (that.value);
        break;

      default:
        break;
    }

    // that is emptied.
    that.type = empty;
  }

  inline
  TParser::stack_symbol_type&
  TParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
      switch (that.type_get ())
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        value.copy< EBinaryOp > (that.value);
        break;

      case 86: // unary-op
        value.copy< EUnaryOp > (that.value);
        break;

      case 31: // "string literal"
        value.copy< Stroka > (that.value);
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        value.copy< TExpressionList > (that.value);
        break;

      case 69: // identifier-list
        value.copy< TIdentifierList > (that.value);
        break;

      case 91: // const-list
      case 92: // const-tuple
        value.copy< TLiteralValueList > (that.value);
        break;

      case 93: // const-tuple-list
        value.copy< TLiteralValueTupleList > (that.value);
        break;

      case 89: // literal-value
      case 90: // const-value
        value.copy< TNullable<TLiteralValue> > (that.value);
        break;

      case 66: // order-expr-list
        value.copy< TOrderExpressionList > (that.value);
        break;

      case 87: // qualified-identifier
        value.copy< TReferenceExpressionPtr > (that.value);
        break;

      case 27: // "identifier"
        value.copy< TStringBuf > (that.value);
        break;

      case 58: // table-descriptor
        value.copy< TTableDescriptor > (that.value);
        break;

      case 61: // is-left
      case 67: // is-desc
        value.copy< bool > (that.value);
        break;

      case 30: // "double literal"
        value.copy< double > (that.value);
        break;

      case 28: // "int64 literal"
        value.copy< i64 > (that.value);
        break;

      case 29: // "uint64 literal"
        value.copy< ui64 > (that.value);
        break;

      default:
        break;
    }

    location = that.location;
    return *this;
  }


  template <typename Base>
  inline
  void
  TParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);
  }

#if YT_QL_YYDEBUG
  template <typename Base>
  void
  TParser::yy_print_ (std::ostream& yyo,
                                     const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YYUSE (yyoutput);
    symbol_number_type yytype = yysym.type_get ();
    yyo << (yytype < yyntokens_ ? "token" : "nterm")
        << ' ' << yytname_[yytype] << " ("
        << yysym.location << ": ";
    YYUSE (yytype);
    yyo << ')';
  }
#endif

  inline
  void
  TParser::yypush_ (const char* m, state_type s, symbol_type& sym)
  {
    stack_symbol_type t (s, sym);
    yypush_ (m, t);
  }

  inline
  void
  TParser::yypush_ (const char* m, stack_symbol_type& s)
  {
    if (m)
      YY_SYMBOL_PRINT (m, s);
    yystack_.push (s);
  }

  inline
  void
  TParser::yypop_ (unsigned int n)
  {
    yystack_.pop (n);
  }

#if YT_QL_YYDEBUG
  std::ostream&
  TParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  TParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  TParser::debug_level_type
  TParser::debug_level () const
  {
    return yydebug_;
  }

  void
  TParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YT_QL_YYDEBUG

  inline TParser::state_type
  TParser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - yyntokens_];
  }

  inline bool
  TParser::yy_pact_value_is_default_ (int yyvalue)
  {
    return yyvalue == yypact_ninf_;
  }

  inline bool
  TParser::yy_table_value_is_error_ (int yyvalue)
  {
    return yyvalue == yytable_ninf_;
  }

  int
  TParser::parse ()
  {
    /// Whether yyla contains a lookahead.
    bool yyempty = true;

    // State.
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

    // FIXME: This shoud be completely indented.  It is not yet to
    // avoid gratuitous conflicts when merging into the master branch.
    try
      {
    YYCDEBUG << "Starting parse" << std::endl;


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULLPTR, 0, yyla);

    // A new symbol was pushed on the stack.
  yynewstate:
    YYCDEBUG << "Entering state " << yystack_[0].state << std::endl;

    // Accept?
    if (yystack_[0].state == yyfinal_)
      goto yyacceptlab;

    goto yybackup;

    // Backup.
  yybackup:

    // Try to take a decision without lookahead.
    yyn = yypact_[yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyempty)
      {
        YYCDEBUG << "Reading a token: ";
        try
          {
            yyla.type = yytranslate_ (yylex (&yyla.value, &yyla.location));
          }
        catch (const syntax_error& yyexc)
          {
            error (yyexc);
            goto yyerrlab1;
          }
        yyempty = false;
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.type_get ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.type_get ())
      goto yydefault;

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Discard the token being shifted.
    yyempty = true;

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", yyn, yyla);
    goto yynewstate;

  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;

  /*-----------------------------.
  | yyreduce -- Do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_(yystack_[yylen].state, yyr1_[yyn]);
      /* Variants are always initialized to an empty instance of the
         correct type. The default '$$ = $1' action is NOT applied
         when using variants.  */
        switch (yyr1_[yyn])
    {
      case 76: // relational-op
      case 81: // additive-op
      case 83: // multiplicative-op
        yylhs.value.build< EBinaryOp > ();
        break;

      case 86: // unary-op
        yylhs.value.build< EUnaryOp > ();
        break;

      case 31: // "string literal"
        yylhs.value.build< Stroka > ();
        break;

      case 70: // expression
      case 71: // or-op-expr
      case 72: // and-op-expr
      case 73: // not-op-expr
      case 74: // equal-op-expr
      case 75: // relational-op-expr
      case 77: // bitor-op-expr
      case 78: // bitand-op-expr
      case 79: // shift-op-expr
      case 80: // additive-op-expr
      case 82: // multiplicative-op-expr
      case 84: // comma-expr
      case 85: // unary-expr
      case 88: // atomic-expr
        yylhs.value.build< TExpressionList > ();
        break;

      case 69: // identifier-list
        yylhs.value.build< TIdentifierList > ();
        break;

      case 91: // const-list
      case 92: // const-tuple
        yylhs.value.build< TLiteralValueList > ();
        break;

      case 93: // const-tuple-list
        yylhs.value.build< TLiteralValueTupleList > ();
        break;

      case 89: // literal-value
      case 90: // const-value
        yylhs.value.build< TNullable<TLiteralValue> > ();
        break;

      case 66: // order-expr-list
        yylhs.value.build< TOrderExpressionList > ();
        break;

      case 87: // qualified-identifier
        yylhs.value.build< TReferenceExpressionPtr > ();
        break;

      case 27: // "identifier"
        yylhs.value.build< TStringBuf > ();
        break;

      case 58: // table-descriptor
        yylhs.value.build< TTableDescriptor > ();
        break;

      case 61: // is-left
      case 67: // is-desc
        yylhs.value.build< bool > ();
        break;

      case 30: // "double literal"
        yylhs.value.build< double > ();
        break;

      case 28: // "int64 literal"
        yylhs.value.build< i64 > ();
        break;

      case 29: // "uint64 literal"
        yylhs.value.build< ui64 > ();
        break;

      default:
        break;
    }


      // Compute the default @$.
      {
        slice<stack_symbol_type, stack_type> slice (yystack_, yylen);
        YYLLOC_DEFAULT (yylhs.location, slice, yylen);
      }

      // Perform the reduction.
      YY_REDUCE_PRINT (yyn);
      try
        {
          switch (yyn)
            {
  case 7:
    {
            if (yystack_[0].value.as< TExpressionList > ().size() != 1) {
                THROW_ERROR_EXCEPTION("Expected scalar expression, got %Qv", GetSource(yylhs.location, source));
            }
            head->first.As<TExpressionPtr>() = yystack_[0].value.as< TExpressionList > ().front();
        }
    break;

  case 8:
    {
            head->first.As<TQuery>().SelectExprs = yystack_[0].value.as< TExpressionList > ();
        }
    break;

  case 9:
    { }
    break;

  case 10:
    {
            yylhs.value.as< TTableDescriptor > () = TTableDescriptor(Stroka(yystack_[1].value.as< TStringBuf > ()), Stroka(yystack_[0].value.as< TStringBuf > ()));
        }
    break;

  case 11:
    {
            yylhs.value.as< TTableDescriptor > () = TTableDescriptor(Stroka(yystack_[2].value.as< TStringBuf > ()), Stroka(yystack_[0].value.as< TStringBuf > ()));
        }
    break;

  case 12:
    {
            yylhs.value.as< TTableDescriptor > () = TTableDescriptor(Stroka(yystack_[0].value.as< TStringBuf > ()), Stroka());
        }
    break;

  case 13:
    {
            head->first.As<TQuery>().Table = yystack_[1].value.as< TTableDescriptor > ();
        }
    break;

  case 14:
    {
            head->first.As<TQuery>().Joins.emplace_back(yystack_[4].value.as< bool > (), yystack_[2].value.as< TTableDescriptor > (), yystack_[0].value.as< TIdentifierList > ());
        }
    break;

  case 15:
    {
            head->first.As<TQuery>().Joins.emplace_back(yystack_[6].value.as< bool > (), yystack_[4].value.as< TTableDescriptor > (), yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 17:
    {
            yylhs.value.as< bool > () = true;
        }
    break;

  case 18:
    {
            yylhs.value.as< bool > () = false;
        }
    break;

  case 19:
    {
            head->first.As<TQuery>().WherePredicate = yystack_[0].value.as< TExpressionList > ();
        }
    break;

  case 21:
    {
            head->first.As<TQuery>().GroupExprs = yystack_[0].value.as< TExpressionList > ();
        }
    break;

  case 23:
    {
            head->first.As<TQuery>().HavingPredicate = yystack_[0].value.as< TExpressionList > ();
        }
    break;

  case 25:
    {
            head->first.As<TQuery>().OrderExpressions = yystack_[0].value.as< TOrderExpressionList > ();
        }
    break;

  case 27:
    {
            yylhs.value.as< TOrderExpressionList > ().swap(yystack_[3].value.as< TOrderExpressionList > ());
            yylhs.value.as< TOrderExpressionList > ().emplace_back(yystack_[1].value.as< TExpressionList > (), yystack_[0].value.as< bool > ());
        }
    break;

  case 28:
    {
            yylhs.value.as< TOrderExpressionList > ().emplace_back(yystack_[1].value.as< TExpressionList > (), yystack_[0].value.as< bool > ());
        }
    break;

  case 29:
    {
            yylhs.value.as< bool > () = true;
        }
    break;

  case 30:
    {
            yylhs.value.as< bool > () = false;
        }
    break;

  case 31:
    {
            yylhs.value.as< bool > () = false;
        }
    break;

  case 32:
    {
            head->first.As<TQuery>().Limit = yystack_[0].value.as< i64 > ();
        }
    break;

  case 34:
    {
            yylhs.value.as< TIdentifierList > ().swap(yystack_[2].value.as< TIdentifierList > ());
            yylhs.value.as< TIdentifierList > ().push_back(yystack_[0].value.as< TReferenceExpressionPtr > ());
        }
    break;

  case 35:
    {
            yylhs.value.as< TIdentifierList > ().push_back(yystack_[0].value.as< TReferenceExpressionPtr > ());
        }
    break;

  case 36:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 37:
    {
            if (yystack_[2].value.as< TExpressionList > ().size() != 1) {
                THROW_ERROR_EXCEPTION("Aliased expression %Qv must be scalar", GetSource(yylhs.location, source));
            }
            auto inserted = head->second.insert(MakePair(Stroka(yystack_[0].value.as< TStringBuf > ()), yystack_[2].value.as< TExpressionList > ().front()));
            if (!inserted.second) {
                THROW_ERROR_EXCEPTION("Alias %Qv has been already used", yystack_[0].value.as< TStringBuf > ());
            }
            yylhs.value.as< TExpressionList > () = MakeExpr<TReferenceExpression>(yylhs.location, yystack_[0].value.as< TStringBuf > ());
        }
    break;

  case 38:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::Or, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 39:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 40:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::And, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 41:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 42:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TUnaryOpExpression>(yylhs.location, EUnaryOp::Not, yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 43:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 44:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::Equal, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 45:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::NotEqual, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 46:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 47:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 48:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::And,
                MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::GreaterOrEqual, yystack_[4].value.as< TExpressionList > (), yystack_[2].value.as< TExpressionList > ()),
                MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::LessOrEqual, yystack_[4].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ()));

        }
    break;

  case 49:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TInExpression>(yylhs.location, yystack_[4].value.as< TExpressionList > (), yystack_[1].value.as< TLiteralValueTupleList > ());
        }
    break;

  case 50:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 51:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Less; }
    break;

  case 52:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::LessOrEqual; }
    break;

  case 53:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Greater; }
    break;

  case 54:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::GreaterOrEqual; }
    break;

  case 55:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::BitOr, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 56:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 57:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::BitAnd, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 58:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 59:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::LeftShift, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 60:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, EBinaryOp::RightShift, yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 61:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 62:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 63:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 64:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Plus; }
    break;

  case 65:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Minus; }
    break;

  case 66:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TBinaryOpExpression>(yylhs.location, yystack_[1].value.as< EBinaryOp > (), yystack_[2].value.as< TExpressionList > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 67:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 68:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Multiply; }
    break;

  case 69:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Divide; }
    break;

  case 70:
    { yylhs.value.as< EBinaryOp > () = EBinaryOp::Modulo; }
    break;

  case 71:
    {
            yylhs.value.as< TExpressionList > () = yystack_[2].value.as< TExpressionList > ();
            yylhs.value.as< TExpressionList > ().insert(yylhs.value.as< TExpressionList > ().end(), yystack_[0].value.as< TExpressionList > ().begin(), yystack_[0].value.as< TExpressionList > ().end());
        }
    break;

  case 72:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 73:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TUnaryOpExpression>(yylhs.location, yystack_[1].value.as< EUnaryOp > (), yystack_[0].value.as< TExpressionList > ());
        }
    break;

  case 74:
    { yylhs.value.as< TExpressionList > () = yystack_[0].value.as< TExpressionList > (); }
    break;

  case 75:
    { yylhs.value.as< EUnaryOp > () = EUnaryOp::Plus; }
    break;

  case 76:
    { yylhs.value.as< EUnaryOp > () = EUnaryOp::Minus; }
    break;

  case 77:
    { yylhs.value.as< EUnaryOp > () = EUnaryOp::BitNot; }
    break;

  case 78:
    {
            yylhs.value.as< TReferenceExpressionPtr > () = New<TReferenceExpression>(yylhs.location, yystack_[0].value.as< TStringBuf > ());
        }
    break;

  case 79:
    {
            yylhs.value.as< TReferenceExpressionPtr > () = New<TReferenceExpression>(yylhs.location, yystack_[0].value.as< TStringBuf > (), yystack_[2].value.as< TStringBuf > ());
        }
    break;

  case 80:
    {
            yylhs.value.as< TExpressionList > () = TExpressionList(1, yystack_[0].value.as< TReferenceExpressionPtr > ());
        }
    break;

  case 81:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TFunctionExpression>(yylhs.location, yystack_[2].value.as< TStringBuf > (), TExpressionList());
        }
    break;

  case 82:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TFunctionExpression>(yylhs.location, yystack_[3].value.as< TStringBuf > (), yystack_[1].value.as< TExpressionList > ());
        }
    break;

  case 83:
    {
            yylhs.value.as< TExpressionList > () = yystack_[1].value.as< TExpressionList > ();
        }
    break;

  case 84:
    {
            yylhs.value.as< TExpressionList > () = MakeExpr<TLiteralExpression>(yylhs.location, *yystack_[0].value.as< TNullable<TLiteralValue> > ());
        }
    break;

  case 85:
    { yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< i64 > (); }
    break;

  case 86:
    { yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< ui64 > (); }
    break;

  case 87:
    { yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< double > (); }
    break;

  case 88:
    { yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< Stroka > (); }
    break;

  case 89:
    { yylhs.value.as< TNullable<TLiteralValue> > () = false; }
    break;

  case 90:
    { yylhs.value.as< TNullable<TLiteralValue> > () = true; }
    break;

  case 91:
    {
            switch (yystack_[1].value.as< EUnaryOp > ()) {
                case EUnaryOp::Minus: {
                    if (auto data = yystack_[0].value.as< TNullable<TLiteralValue> > ()->TryAs<i64>()) {
                        yylhs.value.as< TNullable<TLiteralValue> > () = -*data;
                    } else if (auto data = yystack_[0].value.as< TNullable<TLiteralValue> > ()->TryAs<ui64>()) {
                        yylhs.value.as< TNullable<TLiteralValue> > () = -*data;
                    } else if (auto data = yystack_[0].value.as< TNullable<TLiteralValue> > ()->TryAs<double>()) {
                        yylhs.value.as< TNullable<TLiteralValue> > () = -*data;
                    } else {
                        THROW_ERROR_EXCEPTION("Negation of unsupported type");
                    }
                    break;
                }
                case EUnaryOp::Plus:
                    yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< TNullable<TLiteralValue> > ();
                    break;
                case EUnaryOp::BitNot: {
                    if (auto data = yystack_[0].value.as< TNullable<TLiteralValue> > ()->TryAs<i64>()) {
                        yylhs.value.as< TNullable<TLiteralValue> > () = ~*data;
                    } else if (auto data = yystack_[0].value.as< TNullable<TLiteralValue> > ()->TryAs<ui64>()) {
                        yylhs.value.as< TNullable<TLiteralValue> > () = ~*data;
                    } else {
                        THROW_ERROR_EXCEPTION("Bitwise negation of unsupported type");
                    }
                    break;
                }
                default:
                    YUNREACHABLE();
            }

        }
    break;

  case 92:
    { yylhs.value.as< TNullable<TLiteralValue> > () = yystack_[0].value.as< TNullable<TLiteralValue> > (); }
    break;

  case 93:
    {
            yylhs.value.as< TLiteralValueList > ().swap(yystack_[2].value.as< TLiteralValueList > ());
            yylhs.value.as< TLiteralValueList > ().push_back(*yystack_[0].value.as< TNullable<TLiteralValue> > ());
        }
    break;

  case 94:
    {
            yylhs.value.as< TLiteralValueList > ().push_back(*yystack_[0].value.as< TNullable<TLiteralValue> > ());
        }
    break;

  case 95:
    {
            yylhs.value.as< TLiteralValueList > ().push_back(*yystack_[0].value.as< TNullable<TLiteralValue> > ());
        }
    break;

  case 96:
    {
            yylhs.value.as< TLiteralValueList > () = yystack_[1].value.as< TLiteralValueList > ();
        }
    break;

  case 97:
    {
            yylhs.value.as< TLiteralValueTupleList > ().swap(yystack_[2].value.as< TLiteralValueTupleList > ());
            yylhs.value.as< TLiteralValueTupleList > ().push_back(yystack_[0].value.as< TLiteralValueList > ());
        }
    break;

  case 98:
    {
            yylhs.value.as< TLiteralValueTupleList > ().push_back(yystack_[0].value.as< TLiteralValueList > ());
        }
    break;


            default:
              break;
            }
        }
      catch (const syntax_error& yyexc)
        {
          error (yyexc);
          YYERROR;
        }
      YY_SYMBOL_PRINT ("-> $$ =", yylhs);
      yypop_ (yylen);
      yylen = 0;
      YY_STACK_PRINT ();

      // Shift the result of the reduction.
      yypush_ (YY_NULLPTR, yylhs);
    }
    goto yynewstate;

  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        error (yyla.location, yysyntax_error_ (yystack_[0].state,
                                           yyempty ? yyempty_ : yyla.type_get ()));
      }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.type_get () == yyeof_)
          YYABORT;
        else if (!yyempty)
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyempty = true;
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:

    /* Pacify compilers like GCC when the user code never invokes
       YYERROR and the label yyerrorlab therefore never appears in user
       code.  */
    if (false)
      goto yyerrorlab;
    yyerror_range[1].location = yystack_[yylen - 1].location;
    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    goto yyerrlab1;

  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    {
      stack_symbol_type error_token;
      for (;;)
        {
          yyn = yypact_[yystack_[0].state];
          if (!yy_pact_value_is_default_ (yyn))
            {
              yyn += yyterror_;
              if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == yyterror_)
                {
                  yyn = yytable_[yyn];
                  if (0 < yyn)
                    break;
                }
            }

          // Pop the current state because it cannot handle the error token.
          if (yystack_.size () == 1)
            YYABORT;

          yyerror_range[1].location = yystack_[0].location;
          yy_destroy_ ("Error: popping", yystack_[0]);
          yypop_ ();
          YY_STACK_PRINT ();
        }

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT (error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = yyn;
      yypush_ ("Shifting", error_token);
    }
    goto yynewstate;

    // Accept.
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;

    // Abort.
  yyabortlab:
    yyresult = 1;
    goto yyreturn;

  yyreturn:
    if (!yyempty)
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack"
                 << std::endl;
        // Do not try to display the values of the reclaimed symbols,
        // as their printer might throw an exception.
        if (!yyempty)
          yy_destroy_ (YY_NULLPTR, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULLPTR, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
  }

  void
  TParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what());
  }

  // Generate an error message.
  std::string
  TParser::yysyntax_error_ (state_type yystate, symbol_number_type yytoken) const
  {
    std::string yyres;
    // Number of reported tokens (one for the "unexpected", one per
    // "expected").
    size_t yycount = 0;
    // Its maximum.
    enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
    // Arguments of yyformat.
    char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];

    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yytoken) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state
         merging (from LALR or IELR) and default reductions corrupt the
         expected token list.  However, the list is correct for
         canonical LR with one exception: it will still contain any
         token that will not be accepted due to an error action in a
         later state.
    */
    if (yytoken != yyempty_)
      {
        yyarg[yycount++] = yytname_[yytoken];
        int yyn = yypact_[yystate];
        if (!yy_pact_value_is_default_ (yyn))
          {
            /* Start YYX at -YYN if negative to avoid negative indexes in
               YYCHECK.  In other words, skip the first -YYN actions for
               this state because they are default actions.  */
            int yyxbegin = yyn < 0 ? -yyn : 0;
            // Stay within bounds of both yycheck and yytname.
            int yychecklim = yylast_ - yyn + 1;
            int yyxend = yychecklim < yyntokens_ ? yychecklim : yyntokens_;
            for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
              if (yycheck_[yyx + yyn] == yyx && yyx != yyterror_
                  && !yy_table_value_is_error_ (yytable_[yyx + yyn]))
                {
                  if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                    {
                      yycount = 1;
                      break;
                    }
                  else
                    yyarg[yycount++] = yytname_[yyx];
                }
          }
      }

    char const* yyformat = YY_NULLPTR;
    switch (yycount)
      {
#define YYCASE_(N, S)                         \
        case N:                               \
          yyformat = S;                       \
        break
        YYCASE_(0, YY_("syntax error"));
        YYCASE_(1, YY_("syntax error, unexpected %s"));
        YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
        YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
        YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
        YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
      }

    // Argument number.
    size_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
      if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount)
        {
          yyres += yytnamerr_ (yyarg[yyi++]);
          ++yyp;
        }
      else
        yyres += *yyp;
    return yyres;
  }


  const signed char TParser::yypact_ninf_ = -115;

  const signed char TParser::yytable_ninf_ = -19;

  const short int
  TParser::yypact_[] =
  {
      59,    29,    29,    99,    16,    48,  -115,  -115,     6,  -115,
    -115,  -115,  -115,  -115,    99,  -115,  -115,  -115,  -115,    27,
    -115,    63,    21,  -115,    44,    88,    38,    19,    96,     5,
      50,    41,   128,   137,  -115,  -115,  -115,  -115,    80,  -115,
    -115,  -115,    44,    75,    71,   -24,    83,    80,    92,    99,
      99,    48,    48,  -115,  -115,  -115,  -115,    48,    48,    48,
      48,    48,  -115,  -115,    48,  -115,  -115,  -115,    48,    99,
      48,    77,  -115,    99,  -115,  -115,    69,  -115,  -115,    20,
    -115,   132,  -115,    21,  -115,    88,    88,    38,  -115,    19,
      96,     5,     5,    50,  -115,  -115,   102,   118,   120,  -115,
     133,  -115,    51,    99,   146,    48,    -1,   148,  -115,  -115,
    -115,    70,  -115,  -115,   160,    41,    99,   158,  -115,  -115,
      78,  -115,  -115,   118,    83,   120,    99,   170,  -115,    -1,
    -115,     7,   139,   138,   154,  -115,  -115,   156,    48,    99,
    -115,  -115,  -115,  -115,   140,   143,  -115,   -16,   138,   156,
      48,  -115,  -115,    38
  };

  const unsigned char
  TParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,    89,    90,    78,    85,
      86,    87,    88,    77,     0,     9,    75,    76,     2,     0,
      72,    36,    39,    41,    43,    46,    50,    56,    58,    61,
      63,     8,    67,     0,    80,    74,    84,     3,    20,     4,
       7,     1,    42,     0,     0,     0,     0,    20,     0,     0,
       0,     0,     0,    51,    52,    53,    54,     0,     0,     0,
       0,     0,    64,    65,     0,    70,    68,    69,     0,     0,
       0,     0,    73,     0,     6,    81,     0,    79,    83,    12,
      16,    22,    37,    38,    40,    44,    45,    47,    67,    55,
      57,    59,    60,    62,    66,    71,     0,     0,    19,    82,
       0,    10,    13,     0,    24,     0,     0,     0,    92,    95,
      98,     0,    11,    17,     0,    21,     0,    26,    48,    94,
       0,    91,    49,     0,     0,    23,     0,    33,    96,     0,
      97,     0,    25,    31,     0,     5,    93,     0,     0,     0,
      30,    29,    28,    32,    78,    14,    35,     0,    31,     0,
       0,    27,    34,    15
  };

  const short int
  TParser::yypgoto_[] =
  {
    -115,  -115,  -115,  -115,  -115,   184,    64,  -115,  -115,  -115,
     142,  -115,  -115,  -115,  -115,    39,  -115,  -115,    -3,   -64,
     141,   144,   186,   106,  -115,   -51,   134,   136,   109,  -115,
     129,  -115,    -7,   -56,   -84,  -114,   163,   -86,   -96,  -115,
      74,  -115
  };

  const short int
  TParser::yydefgoto_[] =
  {
      -1,     4,    18,    37,    39,    19,    80,    47,   102,   114,
      74,   104,   117,   127,   132,   142,   135,   145,    20,    21,
      22,    23,    24,    25,    57,    26,    27,    28,    29,    64,
      30,    68,    31,    32,    33,    34,    35,    36,   109,   120,
     110,   111
  };

  const short int
  TParser::yytable_[] =
  {
      40,    88,    88,    88,    88,    88,    87,    45,    88,    98,
     119,   108,    94,   107,    96,    78,    41,    58,    69,   137,
     108,   121,   107,   146,     6,     7,   138,     9,    10,    11,
      12,    13,   150,   136,    46,   152,    76,   108,   100,   107,
      16,    50,    17,   108,    43,   107,    62,   101,    63,   118,
      44,     5,   125,    59,     6,     7,     8,     9,    10,    11,
      12,    13,   -18,     1,     2,     3,    95,    14,   113,    15,
      16,    58,    17,     6,     7,     8,     9,    10,    11,    12,
      13,    48,    88,    69,    49,    65,    14,   147,    73,    16,
      66,    17,    51,    52,    88,    67,   115,     5,    77,   153,
       6,     7,     8,     9,    10,    11,    12,    13,    99,   122,
      79,    69,   123,    14,    75,    97,    16,   128,    17,    82,
     129,     5,   105,   133,     6,     7,     8,     9,    10,    11,
      12,    13,    60,    61,    53,    54,   148,    14,    55,    56,
      16,    49,    17,     6,     7,   103,     9,    10,    11,    12,
      13,    70,    71,   140,   141,   116,   106,    85,    86,    16,
     112,    17,     6,     7,     8,     9,    10,    11,    12,    91,
      92,   124,   126,     6,     7,    14,     9,    10,    11,    12,
     134,   139,   143,   144,    44,   149,    38,   151,   131,    81,
      83,    42,    89,    93,    84,    90,    72,   130
  };

  const unsigned char
  TParser::yycheck_[] =
  {
       3,    57,    58,    59,    60,    61,    57,    14,    64,    73,
     106,    97,    68,    97,    70,    39,     0,    33,    42,    12,
     106,   107,   106,   137,    25,    26,    19,    28,    29,    30,
      31,    32,    48,   129,     7,   149,    43,   123,    18,   123,
      41,    20,    43,   129,    38,   129,    41,    27,    43,   105,
      44,    22,   116,    34,    25,    26,    27,    28,    29,    30,
      31,    32,    11,     4,     5,     6,    69,    38,    17,    40,
      41,    33,    43,    25,    26,    27,    28,    29,    30,    31,
      32,    18,   138,    42,    21,    35,    38,   138,     8,    41,
      40,    43,    48,    49,   150,    45,   103,    22,    27,   150,
      25,    26,    27,    28,    29,    30,    31,    32,    39,    39,
      27,    42,    42,    38,    39,    38,    41,    39,    43,    27,
      42,    22,    20,   126,    25,    26,    27,    28,    29,    30,
      31,    32,    36,    37,    46,    47,   139,    38,    50,    51,
      41,    21,    43,    25,    26,    13,    28,    29,    30,    31,
      32,    23,    24,    15,    16,     9,    38,    51,    52,    41,
      27,    43,    25,    26,    27,    28,    29,    30,    31,    60,
      61,    11,    14,    25,    26,    38,    28,    29,    30,    31,
      10,    42,    28,    27,    44,    42,     2,   148,   124,    47,
      49,     5,    58,    64,    50,    59,    33,   123
  };

  const unsigned char
  TParser::yystos_[] =
  {
       0,     4,     5,     6,    53,    22,    25,    26,    27,    28,
      29,    30,    31,    32,    38,    40,    41,    43,    54,    57,
      70,    71,    72,    73,    74,    75,    77,    78,    79,    80,
      82,    84,    85,    86,    87,    88,    89,    55,    57,    56,
      70,     0,    74,    38,    44,    84,     7,    59,    18,    21,
      20,    48,    49,    46,    47,    50,    51,    76,    33,    34,
      36,    37,    41,    43,    81,    35,    40,    45,    83,    42,
      23,    24,    88,     8,    62,    39,    84,    27,    39,    27,
      58,    62,    27,    72,    73,    75,    75,    77,    85,    78,
      79,    80,    80,    82,    85,    70,    85,    38,    71,    39,
      18,    27,    60,    13,    63,    20,    38,    86,    89,    90,
      92,    93,    27,    17,    61,    84,     9,    64,    85,    90,
      91,    89,    39,    42,    11,    71,    14,    65,    39,    42,
      92,    58,    66,    70,    10,    68,    90,    12,    19,    42,
      15,    16,    67,    28,    27,    69,    87,    77,    70,    42,
      48,    67,    87,    77
  };

  const unsigned char
  TParser::yyr1_[] =
  {
       0,    52,    53,    53,    53,    54,    55,    56,    57,    57,
      58,    58,    58,    59,    60,    60,    60,    61,    61,    62,
      62,    63,    63,    64,    64,    65,    65,    66,    66,    67,
      67,    67,    68,    68,    69,    69,    70,    70,    71,    71,
      72,    72,    73,    73,    74,    74,    74,    75,    75,    75,
      75,    76,    76,    76,    76,    77,    77,    78,    78,    79,
      79,    79,    80,    80,    81,    81,    82,    82,    83,    83,
      83,    84,    84,    85,    85,    86,    86,    86,    87,    87,
      88,    88,    88,    88,    88,    89,    89,    89,    89,    89,
      89,    90,    90,    91,    91,    92,    92,    93,    93
  };

  const unsigned char
  TParser::yyr2_[] =
  {
       0,     2,     2,     2,     2,     7,     2,     1,     1,     1,
       2,     3,     1,     3,     6,     8,     0,     1,     0,     2,
       0,     2,     0,     2,     0,     2,     0,     4,     2,     1,
       1,     0,     2,     0,     3,     1,     1,     3,     3,     1,
       3,     1,     2,     1,     3,     3,     1,     3,     5,     5,
       1,     1,     1,     1,     1,     3,     1,     3,     1,     3,
       3,     1,     3,     1,     1,     1,     3,     1,     1,     1,
       1,     3,     1,     2,     1,     1,     1,     1,     1,     3,
       1,     3,     4,     3,     1,     1,     1,     1,     1,     1,
       1,     2,     1,     3,     1,     1,     3,     3,     1
  };



  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a yyntokens_, nonterminals.
  const char*
  const TParser::yytname_[] =
  {
  "\"end of stream\"", "error", "$undefined", "\"lexer failure\"",
  "StrayWillParseQuery", "StrayWillParseJobQuery",
  "StrayWillParseExpression", "\"keyword `FROM`\"", "\"keyword `WHERE`\"",
  "\"keyword `HAVING`\"", "\"keyword `LIMIT`\"", "\"keyword `JOIN`\"",
  "\"keyword `USING`\"", "\"keyword `GROUP BY`\"",
  "\"keyword `ORDER BY`\"", "\"keyword `ASC`\"", "\"keyword `DESC`\"",
  "\"keyword `LEFT`\"", "\"keyword `AS`\"", "\"keyword `ON`\"",
  "\"keyword `AND`\"", "\"keyword `OR`\"", "\"keyword `NOT`\"",
  "\"keyword `BETWEEN`\"", "\"keyword `IN`\"", "\"keyword `TRUE`\"",
  "\"keyword `FALSE`\"", "\"identifier\"", "\"int64 literal\"",
  "\"uint64 literal\"", "\"double literal\"", "\"string literal\"",
  "\"`~`\"", "\"`|`\"", "\"`&`\"", "\"`%`\"", "\"`<<`\"", "\"`>>`\"",
  "\"`(`\"", "\"`)`\"", "\"`*`\"", "\"`+`\"", "\"`,`\"", "\"`-`\"",
  "\"`.`\"", "\"`/`\"", "\"`<`\"", "\"`<=`\"", "\"`=`\"", "\"`!=`\"",
  "\"`>`\"", "\"`>=`\"", "$accept", "head", "parse-query",
  "parse-job-query", "parse-expression", "select-clause",
  "table-descriptor", "from-clause", "join-clause", "is-left",
  "where-clause", "group-by-clause", "having-clause", "order-by-clause",
  "order-expr-list", "is-desc", "limit-clause", "identifier-list",
  "expression", "or-op-expr", "and-op-expr", "not-op-expr",
  "equal-op-expr", "relational-op-expr", "relational-op", "bitor-op-expr",
  "bitand-op-expr", "shift-op-expr", "additive-op-expr", "additive-op",
  "multiplicative-op-expr", "multiplicative-op", "comma-expr",
  "unary-expr", "unary-op", "qualified-identifier", "atomic-expr",
  "literal-value", "const-value", "const-list", "const-tuple",
  "const-tuple-list", YY_NULLPTR
  };

#if YT_QL_YYDEBUG
  const unsigned short int
  TParser::yyrline_[] =
  {
       0,   162,   162,   163,   164,   168,   172,   176,   186,   190,
     195,   199,   203,   210,   217,   221,   225,   229,   234,   240,
     244,   248,   252,   256,   260,   264,   268,   272,   277,   284,
     288,   293,   299,   303,   307,   312,   319,   321,   335,   339,
     345,   349,   354,   358,   363,   368,   372,   377,   381,   388,
     392,   397,   399,   401,   403,   408,   412,   417,   421,   426,
     430,   434,   439,   443,   448,   450,   455,   459,   464,   466,
     468,   473,   478,   483,   487,   492,   494,   496,   501,   505,
     512,   516,   520,   524,   528,   535,   537,   539,   541,   543,
     545,   550,   583,   588,   593,   600,   604,   611,   616
  };

  // Print the state stack on the debug stream.
  void
  TParser::yystack_print_ ()
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << i->state;
    *yycdebug_ << std::endl;
  }

  // Report on the debug stream that the rule \a yyrule is going to be reduced.
  void
  TParser::yy_reduce_print_ (int yyrule)
  {
    unsigned int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):" << std::endl;
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YT_QL_YYDEBUG

  // Symbol number corresponding to token number t.
  inline
  TParser::token_number_type
  TParser::yytranslate_ (int t)
  {
    static
    const token_number_type
    translate_table[] =
    {
     0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,    35,    34,     2,
      38,    39,    40,    41,    42,    43,    44,    45,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      46,    48,    50,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,    33,     2,    32,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     3,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     6,     5,     4,
       1,     2,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    36,    37,    47,
      49,    51
    };
    const unsigned int user_token_number_max_ = 1031;
    const token_number_type undef_token_ = 2;

    if (static_cast<int>(t) <= yyeof_)
      return yyeof_;
    else if (static_cast<unsigned int> (t) <= user_token_number_max_)
      return translate_table[t];
    else
      return undef_token_;
  }

} } } // NYT::NQueryClient::NAst


#include <core/misc/format.h>

namespace NYT {
namespace NQueryClient {
namespace NAst {

////////////////////////////////////////////////////////////////////////////////

void TParser::error(const location_type& location, const std::string& message)
{
    Stroka mark;
    for (int index = 0; index <= location.second; ++index) {
        mark += index < location.first ? ' ' : '^';
    }
    THROW_ERROR_EXCEPTION("Error while parsing query: %v", message)
        << TErrorAttribute("position", Format("%v-%v", location.first, location.second))
        << TErrorAttribute("query", Format("\n%v\n%v", source, mark));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NAst
} // namespace NQueryClient
} // namespace NYT
