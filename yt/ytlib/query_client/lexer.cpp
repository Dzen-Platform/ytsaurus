
#include <ytlib/query_client/lexer.h>

#include <util/system/defaults.h>
#include <util/string/cast.h>
#include <util/string/escape.h>

namespace NYT {
namespace NQueryClient {
namespace NAst {

////////////////////////////////////////////////////////////////////////////////

typedef TParser::token_type TToken;

////////////////////////////////////////////////////////////////////////////////




namespace {

static const int Lexer_start = 12;
static const int Lexer_first_final = 12;
static const int Lexer_error = 0;

static const int Lexer_en_quoted_identifier = 71;
static const int Lexer_en_main = 12;


} // namespace anonymous

TLexer::TLexer(
    const Stroka& source,
    TParser::token_type strayToken)
    : StrayToken_(strayToken)
    , InjectedStrayToken_(false)
    , p(nullptr)
    , pe(nullptr)
    , eof(nullptr)
    , rs(nullptr)
    , re(nullptr)
    , rd(0)
    , s(nullptr)
{
    Initialize(source.c_str(), source.c_str() + source.length());
}

void TLexer::Initialize(const char* begin, const char* end)
{
    p = s = begin;
    pe = eof = end;

    rs = re = nullptr;
    rd = 0;

    
	{
	cs = Lexer_start;
	ts = 0;
	te = 0;
	act = 0;
	}

}

TParser::token_type TLexer::GetNextToken(
    TParser::semantic_type* value,
    TParser::location_type* location
)
{
    if (!InjectedStrayToken_) {
        InjectedStrayToken_ = true;
        location->first = 0;
        location->second = 0;
        return StrayToken_;
    }

    TParser::token_type type = TToken::End;

    location->first = p - s;
    
	{
	if ( p == pe )
		goto _test_eof;
	goto _resume;

_again:
	switch ( cs ) {
		case 12: goto st12;
		case 0: goto st0;
		case 13: goto st13;
		case 1: goto st1;
		case 2: goto st2;
		case 3: goto st3;
		case 4: goto st4;
		case 5: goto st5;
		case 14: goto st14;
		case 15: goto st15;
		case 6: goto st6;
		case 7: goto st7;
		case 16: goto st16;
		case 17: goto st17;
		case 18: goto st18;
		case 19: goto st19;
		case 20: goto st20;
		case 21: goto st21;
		case 22: goto st22;
		case 23: goto st23;
		case 24: goto st24;
		case 25: goto st25;
		case 26: goto st26;
		case 27: goto st27;
		case 28: goto st28;
		case 29: goto st29;
		case 30: goto st30;
		case 31: goto st31;
		case 32: goto st32;
		case 33: goto st33;
		case 34: goto st34;
		case 35: goto st35;
		case 36: goto st36;
		case 37: goto st37;
		case 38: goto st38;
		case 39: goto st39;
		case 8: goto st8;
		case 9: goto st9;
		case 40: goto st40;
		case 41: goto st41;
		case 42: goto st42;
		case 43: goto st43;
		case 44: goto st44;
		case 45: goto st45;
		case 46: goto st46;
		case 47: goto st47;
		case 48: goto st48;
		case 49: goto st49;
		case 50: goto st50;
		case 51: goto st51;
		case 52: goto st52;
		case 53: goto st53;
		case 54: goto st54;
		case 55: goto st55;
		case 56: goto st56;
		case 57: goto st57;
		case 58: goto st58;
		case 59: goto st59;
		case 10: goto st10;
		case 11: goto st11;
		case 60: goto st60;
		case 61: goto st61;
		case 62: goto st62;
		case 63: goto st63;
		case 64: goto st64;
		case 65: goto st65;
		case 66: goto st66;
		case 67: goto st67;
		case 68: goto st68;
		case 69: goto st69;
		case 70: goto st70;
		case 71: goto st71;
	default: break;
	}

	if ( ++p == pe )
		goto _test_eof;
_resume:
	switch ( cs )
	{
tr0:
	{te = p+1;{ type = TToken::OpNotEqual; {p++; cs = 12; goto _out;} }}
	goto st12;
tr3:
	{te = p+1;{
            type = TToken::StringLiteral;
            value->build(UnescapeC(ts + 1, te - ts - 2));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr7:
	{{p = ((te))-1;}{
            type = TToken::DoubleLiteral;
            value->build(FromString<double>(ts, te - ts));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr10:
	{{p = ((te))-1;}{
            type = TToken::Identifier;
            value->build(TStringBuf(ts, te));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr13:
	{te = p+1;{ type = TToken::KwGroupBy; {p++; cs = 12; goto _out;} }}
	goto st12;
tr16:
	{te = p+1;{ type = TToken::KwOrderBy; {p++; cs = 12; goto _out;} }}
	goto st12;
tr17:
	{te = p+1;{ type = TToken::End; {p++; cs = 12; goto _out;} }}
	goto st12;
tr20:
	{te = p+1;{
            type = static_cast<TToken>((*p));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr39:
	{te = p+1;{
            p--;
            {goto st71;}
        }}
	goto st12;
tr40:
	{te = p+1;{
            YUNREACHABLE();
        }}
	goto st12;
tr41:
	{te = p;p--;{ location->first = te - s; }}
	goto st12;
tr42:
	{te = p;p--;{
            type = static_cast<TToken>((*p));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr44:
	{te = p;p--;{
            type = TToken::DoubleLiteral;
            value->build(FromString<double>(ts, te - ts));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr46:
	{te = p;p--;{
            type = TToken::Int64Literal;
            value->build(FromString<ui64>(ts, te - ts));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr47:
	{te = p+1;{
            type = TToken::Uint64Literal;
            value->build(FromString<ui64>(ts, te - ts - 1));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr48:
	{te = p+1;{ type = TToken::OpLessOrEqual; {p++; cs = 12; goto _out;} }}
	goto st12;
tr49:
	{te = p+1;{ type = TToken::OpGreaterOrEqual; {p++; cs = 12; goto _out;} }}
	goto st12;
tr50:
	{te = p;p--;{
            type = TToken::Identifier;
            value->build(TStringBuf(ts, te));
            {p++; cs = 12; goto _out;}
        }}
	goto st12;
tr53:
	{	switch( act ) {
	case 4:
	{{p = ((te))-1;} type = TToken::KwFrom; {p++; cs = 12; goto _out;} }
	break;
	case 5:
	{{p = ((te))-1;} type = TToken::KwWhere; {p++; cs = 12; goto _out;} }
	break;
	case 6:
	{{p = ((te))-1;} type = TToken::KwHaving; {p++; cs = 12; goto _out;} }
	break;
	case 7:
	{{p = ((te))-1;} type = TToken::KwLimit; {p++; cs = 12; goto _out;} }
	break;
	case 8:
	{{p = ((te))-1;} type = TToken::KwJoin; {p++; cs = 12; goto _out;} }
	break;
	case 9:
	{{p = ((te))-1;} type = TToken::KwUsing; {p++; cs = 12; goto _out;} }
	break;
	case 12:
	{{p = ((te))-1;} type = TToken::KwAs; {p++; cs = 12; goto _out;} }
	break;
	case 13:
	{{p = ((te))-1;} type = TToken::KwOn; {p++; cs = 12; goto _out;} }
	break;
	case 14:
	{{p = ((te))-1;} type = TToken::KwAnd; {p++; cs = 12; goto _out;} }
	break;
	case 16:
	{{p = ((te))-1;} type = TToken::KwNot; {p++; cs = 12; goto _out;} }
	break;
	case 17:
	{{p = ((te))-1;} type = TToken::KwBetween; {p++; cs = 12; goto _out;} }
	break;
	case 18:
	{{p = ((te))-1;} type = TToken::KwIn; {p++; cs = 12; goto _out;} }
	break;
	case 19:
	{{p = ((te))-1;} type = TToken::KwFalse; {p++; cs = 12; goto _out;} }
	break;
	case 20:
	{{p = ((te))-1;} type = TToken::KwTrue; {p++; cs = 12; goto _out;} }
	break;
	case 21:
	{{p = ((te))-1;}
            type = TToken::Identifier;
            value->build(TStringBuf(ts, te));
            {p++; cs = 12; goto _out;}
        }
	break;
	}
	}
	goto st12;
tr89:
	{te = p;p--;{ type = TToken::KwOr; {p++; cs = 12; goto _out;} }}
	goto st12;
st12:
	{ts = 0;}
	if ( ++p == pe )
		goto _test_eof12;
case 12:
	{ts = p;}
	switch( (*p) ) {
		case 0: goto tr17;
		case 32: goto st13;
		case 33: goto st1;
		case 34: goto st2;
		case 37: goto tr20;
		case 39: goto st4;
		case 46: goto st14;
		case 60: goto st18;
		case 61: goto tr20;
		case 62: goto st19;
		case 65: goto st20;
		case 66: goto st23;
		case 70: goto st29;
		case 71: goto st35;
		case 72: goto st40;
		case 73: goto st45;
		case 74: goto st46;
		case 76: goto st49;
		case 78: goto st53;
		case 79: goto st55;
		case 84: goto st60;
		case 85: goto st63;
		case 87: goto st67;
		case 91: goto tr39;
		case 93: goto tr40;
		case 95: goto tr27;
		case 97: goto st20;
		case 98: goto st23;
		case 102: goto st29;
		case 103: goto st35;
		case 104: goto st40;
		case 105: goto st45;
		case 106: goto st46;
		case 108: goto st49;
		case 110: goto st53;
		case 111: goto st55;
		case 116: goto st60;
		case 117: goto st63;
		case 119: goto st67;
	}
	if ( (*p) < 48 ) {
		if ( (*p) > 13 ) {
			if ( 40 <= (*p) && (*p) <= 47 )
				goto tr20;
		} else if ( (*p) >= 9 )
			goto st13;
	} else if ( (*p) > 57 ) {
		if ( (*p) > 90 ) {
			if ( 99 <= (*p) && (*p) <= 122 )
				goto tr27;
		} else if ( (*p) >= 67 )
			goto tr27;
	} else
		goto st17;
	goto st0;
st0:
cs = 0;
	goto _out;
st13:
	if ( ++p == pe )
		goto _test_eof13;
case 13:
	if ( (*p) == 32 )
		goto st13;
	if ( 9 <= (*p) && (*p) <= 13 )
		goto st13;
	goto tr41;
st1:
	if ( ++p == pe )
		goto _test_eof1;
case 1:
	if ( (*p) == 61 )
		goto tr0;
	goto st0;
st2:
	if ( ++p == pe )
		goto _test_eof2;
case 2:
	switch( (*p) ) {
		case 34: goto tr3;
		case 92: goto st3;
	}
	goto st2;
st3:
	if ( ++p == pe )
		goto _test_eof3;
case 3:
	goto st2;
st4:
	if ( ++p == pe )
		goto _test_eof4;
case 4:
	switch( (*p) ) {
		case 39: goto tr3;
		case 92: goto st5;
	}
	goto st4;
st5:
	if ( ++p == pe )
		goto _test_eof5;
case 5:
	goto st4;
st14:
	if ( ++p == pe )
		goto _test_eof14;
case 14:
	if ( 48 <= (*p) && (*p) <= 57 )
		goto tr43;
	goto tr42;
tr43:
	{te = p+1;}
	goto st15;
st15:
	if ( ++p == pe )
		goto _test_eof15;
case 15:
	switch( (*p) ) {
		case 69: goto st6;
		case 101: goto st6;
	}
	if ( 48 <= (*p) && (*p) <= 57 )
		goto tr43;
	goto tr44;
st6:
	if ( ++p == pe )
		goto _test_eof6;
case 6:
	switch( (*p) ) {
		case 43: goto st7;
		case 45: goto st7;
	}
	if ( 48 <= (*p) && (*p) <= 57 )
		goto st16;
	goto tr7;
st7:
	if ( ++p == pe )
		goto _test_eof7;
case 7:
	if ( 48 <= (*p) && (*p) <= 57 )
		goto st16;
	goto tr7;
st16:
	if ( ++p == pe )
		goto _test_eof16;
case 16:
	if ( 48 <= (*p) && (*p) <= 57 )
		goto st16;
	goto tr44;
st17:
	if ( ++p == pe )
		goto _test_eof17;
case 17:
	switch( (*p) ) {
		case 46: goto tr43;
		case 117: goto tr47;
	}
	if ( 48 <= (*p) && (*p) <= 57 )
		goto st17;
	goto tr46;
st18:
	if ( ++p == pe )
		goto _test_eof18;
case 18:
	if ( (*p) == 61 )
		goto tr48;
	goto tr42;
st19:
	if ( ++p == pe )
		goto _test_eof19;
case 19:
	if ( (*p) == 61 )
		goto tr49;
	goto tr42;
st20:
	if ( ++p == pe )
		goto _test_eof20;
case 20:
	switch( (*p) ) {
		case 78: goto st22;
		case 83: goto tr52;
		case 95: goto tr27;
		case 110: goto st22;
		case 115: goto tr52;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
tr27:
	{te = p+1;}
	{act = 21;}
	goto st21;
tr52:
	{te = p+1;}
	{act = 12;}
	goto st21;
tr54:
	{te = p+1;}
	{act = 14;}
	goto st21;
tr60:
	{te = p+1;}
	{act = 17;}
	goto st21;
tr65:
	{te = p+1;}
	{act = 19;}
	goto st21;
tr67:
	{te = p+1;}
	{act = 4;}
	goto st21;
tr76:
	{te = p+1;}
	{act = 6;}
	goto st21;
tr77:
	{te = p+1;}
	{act = 18;}
	goto st21;
tr80:
	{te = p+1;}
	{act = 8;}
	goto st21;
tr84:
	{te = p+1;}
	{act = 7;}
	goto st21;
tr86:
	{te = p+1;}
	{act = 16;}
	goto st21;
tr87:
	{te = p+1;}
	{act = 13;}
	goto st21;
tr95:
	{te = p+1;}
	{act = 20;}
	goto st21;
tr99:
	{te = p+1;}
	{act = 9;}
	goto st21;
tr103:
	{te = p+1;}
	{act = 5;}
	goto st21;
st21:
	if ( ++p == pe )
		goto _test_eof21;
case 21:
	if ( (*p) == 95 )
		goto tr27;
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr53;
st22:
	if ( ++p == pe )
		goto _test_eof22;
case 22:
	switch( (*p) ) {
		case 68: goto tr54;
		case 95: goto tr27;
		case 100: goto tr54;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st23:
	if ( ++p == pe )
		goto _test_eof23;
case 23:
	switch( (*p) ) {
		case 69: goto st24;
		case 95: goto tr27;
		case 101: goto st24;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st24:
	if ( ++p == pe )
		goto _test_eof24;
case 24:
	switch( (*p) ) {
		case 84: goto st25;
		case 95: goto tr27;
		case 116: goto st25;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st25:
	if ( ++p == pe )
		goto _test_eof25;
case 25:
	switch( (*p) ) {
		case 87: goto st26;
		case 95: goto tr27;
		case 119: goto st26;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st26:
	if ( ++p == pe )
		goto _test_eof26;
case 26:
	switch( (*p) ) {
		case 69: goto st27;
		case 95: goto tr27;
		case 101: goto st27;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st27:
	if ( ++p == pe )
		goto _test_eof27;
case 27:
	switch( (*p) ) {
		case 69: goto st28;
		case 95: goto tr27;
		case 101: goto st28;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st28:
	if ( ++p == pe )
		goto _test_eof28;
case 28:
	switch( (*p) ) {
		case 78: goto tr60;
		case 95: goto tr27;
		case 110: goto tr60;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st29:
	if ( ++p == pe )
		goto _test_eof29;
case 29:
	switch( (*p) ) {
		case 65: goto st30;
		case 82: goto st33;
		case 95: goto tr27;
		case 97: goto st30;
		case 114: goto st33;
	}
	if ( (*p) < 66 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 98 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st30:
	if ( ++p == pe )
		goto _test_eof30;
case 30:
	switch( (*p) ) {
		case 76: goto st31;
		case 95: goto tr27;
		case 108: goto st31;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st31:
	if ( ++p == pe )
		goto _test_eof31;
case 31:
	switch( (*p) ) {
		case 83: goto st32;
		case 95: goto tr27;
		case 115: goto st32;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st32:
	if ( ++p == pe )
		goto _test_eof32;
case 32:
	switch( (*p) ) {
		case 69: goto tr65;
		case 95: goto tr27;
		case 101: goto tr65;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st33:
	if ( ++p == pe )
		goto _test_eof33;
case 33:
	switch( (*p) ) {
		case 79: goto st34;
		case 95: goto tr27;
		case 111: goto st34;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st34:
	if ( ++p == pe )
		goto _test_eof34;
case 34:
	switch( (*p) ) {
		case 77: goto tr67;
		case 95: goto tr27;
		case 109: goto tr67;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st35:
	if ( ++p == pe )
		goto _test_eof35;
case 35:
	switch( (*p) ) {
		case 82: goto st36;
		case 95: goto tr27;
		case 114: goto st36;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st36:
	if ( ++p == pe )
		goto _test_eof36;
case 36:
	switch( (*p) ) {
		case 79: goto st37;
		case 95: goto tr27;
		case 111: goto st37;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st37:
	if ( ++p == pe )
		goto _test_eof37;
case 37:
	switch( (*p) ) {
		case 85: goto st38;
		case 95: goto tr27;
		case 117: goto st38;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st38:
	if ( ++p == pe )
		goto _test_eof38;
case 38:
	switch( (*p) ) {
		case 80: goto tr71;
		case 95: goto tr27;
		case 112: goto tr71;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
tr71:
	{te = p+1;}
	goto st39;
st39:
	if ( ++p == pe )
		goto _test_eof39;
case 39:
	switch( (*p) ) {
		case 32: goto st8;
		case 95: goto tr27;
	}
	if ( (*p) < 48 ) {
		if ( 9 <= (*p) && (*p) <= 13 )
			goto st8;
	} else if ( (*p) > 57 ) {
		if ( (*p) > 90 ) {
			if ( 97 <= (*p) && (*p) <= 122 )
				goto tr27;
		} else if ( (*p) >= 65 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st8:
	if ( ++p == pe )
		goto _test_eof8;
case 8:
	switch( (*p) ) {
		case 32: goto st8;
		case 66: goto st9;
		case 98: goto st9;
	}
	if ( 9 <= (*p) && (*p) <= 13 )
		goto st8;
	goto tr10;
st9:
	if ( ++p == pe )
		goto _test_eof9;
case 9:
	switch( (*p) ) {
		case 89: goto tr13;
		case 121: goto tr13;
	}
	goto tr10;
st40:
	if ( ++p == pe )
		goto _test_eof40;
case 40:
	switch( (*p) ) {
		case 65: goto st41;
		case 95: goto tr27;
		case 97: goto st41;
	}
	if ( (*p) < 66 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 98 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st41:
	if ( ++p == pe )
		goto _test_eof41;
case 41:
	switch( (*p) ) {
		case 86: goto st42;
		case 95: goto tr27;
		case 118: goto st42;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st42:
	if ( ++p == pe )
		goto _test_eof42;
case 42:
	switch( (*p) ) {
		case 73: goto st43;
		case 95: goto tr27;
		case 105: goto st43;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st43:
	if ( ++p == pe )
		goto _test_eof43;
case 43:
	switch( (*p) ) {
		case 78: goto st44;
		case 95: goto tr27;
		case 110: goto st44;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st44:
	if ( ++p == pe )
		goto _test_eof44;
case 44:
	switch( (*p) ) {
		case 71: goto tr76;
		case 95: goto tr27;
		case 103: goto tr76;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st45:
	if ( ++p == pe )
		goto _test_eof45;
case 45:
	switch( (*p) ) {
		case 78: goto tr77;
		case 95: goto tr27;
		case 110: goto tr77;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st46:
	if ( ++p == pe )
		goto _test_eof46;
case 46:
	switch( (*p) ) {
		case 79: goto st47;
		case 95: goto tr27;
		case 111: goto st47;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st47:
	if ( ++p == pe )
		goto _test_eof47;
case 47:
	switch( (*p) ) {
		case 73: goto st48;
		case 95: goto tr27;
		case 105: goto st48;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st48:
	if ( ++p == pe )
		goto _test_eof48;
case 48:
	switch( (*p) ) {
		case 78: goto tr80;
		case 95: goto tr27;
		case 110: goto tr80;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st49:
	if ( ++p == pe )
		goto _test_eof49;
case 49:
	switch( (*p) ) {
		case 73: goto st50;
		case 95: goto tr27;
		case 105: goto st50;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st50:
	if ( ++p == pe )
		goto _test_eof50;
case 50:
	switch( (*p) ) {
		case 77: goto st51;
		case 95: goto tr27;
		case 109: goto st51;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st51:
	if ( ++p == pe )
		goto _test_eof51;
case 51:
	switch( (*p) ) {
		case 73: goto st52;
		case 95: goto tr27;
		case 105: goto st52;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st52:
	if ( ++p == pe )
		goto _test_eof52;
case 52:
	switch( (*p) ) {
		case 84: goto tr84;
		case 95: goto tr27;
		case 116: goto tr84;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st53:
	if ( ++p == pe )
		goto _test_eof53;
case 53:
	switch( (*p) ) {
		case 79: goto st54;
		case 95: goto tr27;
		case 111: goto st54;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st54:
	if ( ++p == pe )
		goto _test_eof54;
case 54:
	switch( (*p) ) {
		case 84: goto tr86;
		case 95: goto tr27;
		case 116: goto tr86;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st55:
	if ( ++p == pe )
		goto _test_eof55;
case 55:
	switch( (*p) ) {
		case 78: goto tr87;
		case 82: goto st56;
		case 95: goto tr27;
		case 110: goto tr87;
		case 114: goto st56;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st56:
	if ( ++p == pe )
		goto _test_eof56;
case 56:
	switch( (*p) ) {
		case 68: goto st57;
		case 95: goto tr27;
		case 100: goto st57;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr89;
st57:
	if ( ++p == pe )
		goto _test_eof57;
case 57:
	switch( (*p) ) {
		case 69: goto st58;
		case 95: goto tr27;
		case 101: goto st58;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st58:
	if ( ++p == pe )
		goto _test_eof58;
case 58:
	switch( (*p) ) {
		case 82: goto tr92;
		case 95: goto tr27;
		case 114: goto tr92;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
tr92:
	{te = p+1;}
	goto st59;
st59:
	if ( ++p == pe )
		goto _test_eof59;
case 59:
	switch( (*p) ) {
		case 32: goto st10;
		case 95: goto tr27;
	}
	if ( (*p) < 48 ) {
		if ( 9 <= (*p) && (*p) <= 13 )
			goto st10;
	} else if ( (*p) > 57 ) {
		if ( (*p) > 90 ) {
			if ( 97 <= (*p) && (*p) <= 122 )
				goto tr27;
		} else if ( (*p) >= 65 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st10:
	if ( ++p == pe )
		goto _test_eof10;
case 10:
	switch( (*p) ) {
		case 32: goto st10;
		case 66: goto st11;
		case 98: goto st11;
	}
	if ( 9 <= (*p) && (*p) <= 13 )
		goto st10;
	goto tr10;
st11:
	if ( ++p == pe )
		goto _test_eof11;
case 11:
	switch( (*p) ) {
		case 89: goto tr16;
		case 121: goto tr16;
	}
	goto tr10;
st60:
	if ( ++p == pe )
		goto _test_eof60;
case 60:
	switch( (*p) ) {
		case 82: goto st61;
		case 95: goto tr27;
		case 114: goto st61;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st61:
	if ( ++p == pe )
		goto _test_eof61;
case 61:
	switch( (*p) ) {
		case 85: goto st62;
		case 95: goto tr27;
		case 117: goto st62;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st62:
	if ( ++p == pe )
		goto _test_eof62;
case 62:
	switch( (*p) ) {
		case 69: goto tr95;
		case 95: goto tr27;
		case 101: goto tr95;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st63:
	if ( ++p == pe )
		goto _test_eof63;
case 63:
	switch( (*p) ) {
		case 83: goto st64;
		case 95: goto tr27;
		case 115: goto st64;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st64:
	if ( ++p == pe )
		goto _test_eof64;
case 64:
	switch( (*p) ) {
		case 73: goto st65;
		case 95: goto tr27;
		case 105: goto st65;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st65:
	if ( ++p == pe )
		goto _test_eof65;
case 65:
	switch( (*p) ) {
		case 78: goto st66;
		case 95: goto tr27;
		case 110: goto st66;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st66:
	if ( ++p == pe )
		goto _test_eof66;
case 66:
	switch( (*p) ) {
		case 71: goto tr99;
		case 95: goto tr27;
		case 103: goto tr99;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st67:
	if ( ++p == pe )
		goto _test_eof67;
case 67:
	switch( (*p) ) {
		case 72: goto st68;
		case 95: goto tr27;
		case 104: goto st68;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st68:
	if ( ++p == pe )
		goto _test_eof68;
case 68:
	switch( (*p) ) {
		case 69: goto st69;
		case 95: goto tr27;
		case 101: goto st69;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st69:
	if ( ++p == pe )
		goto _test_eof69;
case 69:
	switch( (*p) ) {
		case 82: goto st70;
		case 95: goto tr27;
		case 114: goto st70;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
st70:
	if ( ++p == pe )
		goto _test_eof70;
case 70:
	switch( (*p) ) {
		case 69: goto tr103;
		case 95: goto tr27;
		case 101: goto tr103;
	}
	if ( (*p) < 65 ) {
		if ( 48 <= (*p) && (*p) <= 57 )
			goto tr27;
	} else if ( (*p) > 90 ) {
		if ( 97 <= (*p) && (*p) <= 122 )
			goto tr27;
	} else
		goto tr27;
	goto tr50;
tr104:
	{te = p+1;}
	goto st71;
tr105:
	{te = p+1;{
            if (++rd == 1) {
                rs = p + 1;
            }
        }}
	goto st71;
tr106:
	cs = 71;
	{te = p+1;{
            if (--rd == 0) {
                re = p;
                type = TToken::Identifier;
                value->build(TStringBuf(rs, re));
                cs = 12;
                {p++; goto _out;}
            }
        }}
	goto _again;
st71:
	{ts = 0;}
	if ( ++p == pe )
		goto _test_eof71;
case 71:
	{ts = p;}
	switch( (*p) ) {
		case 0: goto st0;
		case 91: goto tr105;
		case 93: goto tr106;
	}
	goto tr104;
	}
	_test_eof12: cs = 12; goto _test_eof; 
	_test_eof13: cs = 13; goto _test_eof; 
	_test_eof1: cs = 1; goto _test_eof; 
	_test_eof2: cs = 2; goto _test_eof; 
	_test_eof3: cs = 3; goto _test_eof; 
	_test_eof4: cs = 4; goto _test_eof; 
	_test_eof5: cs = 5; goto _test_eof; 
	_test_eof14: cs = 14; goto _test_eof; 
	_test_eof15: cs = 15; goto _test_eof; 
	_test_eof6: cs = 6; goto _test_eof; 
	_test_eof7: cs = 7; goto _test_eof; 
	_test_eof16: cs = 16; goto _test_eof; 
	_test_eof17: cs = 17; goto _test_eof; 
	_test_eof18: cs = 18; goto _test_eof; 
	_test_eof19: cs = 19; goto _test_eof; 
	_test_eof20: cs = 20; goto _test_eof; 
	_test_eof21: cs = 21; goto _test_eof; 
	_test_eof22: cs = 22; goto _test_eof; 
	_test_eof23: cs = 23; goto _test_eof; 
	_test_eof24: cs = 24; goto _test_eof; 
	_test_eof25: cs = 25; goto _test_eof; 
	_test_eof26: cs = 26; goto _test_eof; 
	_test_eof27: cs = 27; goto _test_eof; 
	_test_eof28: cs = 28; goto _test_eof; 
	_test_eof29: cs = 29; goto _test_eof; 
	_test_eof30: cs = 30; goto _test_eof; 
	_test_eof31: cs = 31; goto _test_eof; 
	_test_eof32: cs = 32; goto _test_eof; 
	_test_eof33: cs = 33; goto _test_eof; 
	_test_eof34: cs = 34; goto _test_eof; 
	_test_eof35: cs = 35; goto _test_eof; 
	_test_eof36: cs = 36; goto _test_eof; 
	_test_eof37: cs = 37; goto _test_eof; 
	_test_eof38: cs = 38; goto _test_eof; 
	_test_eof39: cs = 39; goto _test_eof; 
	_test_eof8: cs = 8; goto _test_eof; 
	_test_eof9: cs = 9; goto _test_eof; 
	_test_eof40: cs = 40; goto _test_eof; 
	_test_eof41: cs = 41; goto _test_eof; 
	_test_eof42: cs = 42; goto _test_eof; 
	_test_eof43: cs = 43; goto _test_eof; 
	_test_eof44: cs = 44; goto _test_eof; 
	_test_eof45: cs = 45; goto _test_eof; 
	_test_eof46: cs = 46; goto _test_eof; 
	_test_eof47: cs = 47; goto _test_eof; 
	_test_eof48: cs = 48; goto _test_eof; 
	_test_eof49: cs = 49; goto _test_eof; 
	_test_eof50: cs = 50; goto _test_eof; 
	_test_eof51: cs = 51; goto _test_eof; 
	_test_eof52: cs = 52; goto _test_eof; 
	_test_eof53: cs = 53; goto _test_eof; 
	_test_eof54: cs = 54; goto _test_eof; 
	_test_eof55: cs = 55; goto _test_eof; 
	_test_eof56: cs = 56; goto _test_eof; 
	_test_eof57: cs = 57; goto _test_eof; 
	_test_eof58: cs = 58; goto _test_eof; 
	_test_eof59: cs = 59; goto _test_eof; 
	_test_eof10: cs = 10; goto _test_eof; 
	_test_eof11: cs = 11; goto _test_eof; 
	_test_eof60: cs = 60; goto _test_eof; 
	_test_eof61: cs = 61; goto _test_eof; 
	_test_eof62: cs = 62; goto _test_eof; 
	_test_eof63: cs = 63; goto _test_eof; 
	_test_eof64: cs = 64; goto _test_eof; 
	_test_eof65: cs = 65; goto _test_eof; 
	_test_eof66: cs = 66; goto _test_eof; 
	_test_eof67: cs = 67; goto _test_eof; 
	_test_eof68: cs = 68; goto _test_eof; 
	_test_eof69: cs = 69; goto _test_eof; 
	_test_eof70: cs = 70; goto _test_eof; 
	_test_eof71: cs = 71; goto _test_eof; 

	_test_eof: {}
	if ( p == eof )
	{
	switch ( cs ) {
	case 13: goto tr41;
	case 14: goto tr42;
	case 15: goto tr44;
	case 6: goto tr7;
	case 7: goto tr7;
	case 16: goto tr44;
	case 17: goto tr46;
	case 18: goto tr42;
	case 19: goto tr42;
	case 20: goto tr50;
	case 21: goto tr53;
	case 22: goto tr50;
	case 23: goto tr50;
	case 24: goto tr50;
	case 25: goto tr50;
	case 26: goto tr50;
	case 27: goto tr50;
	case 28: goto tr50;
	case 29: goto tr50;
	case 30: goto tr50;
	case 31: goto tr50;
	case 32: goto tr50;
	case 33: goto tr50;
	case 34: goto tr50;
	case 35: goto tr50;
	case 36: goto tr50;
	case 37: goto tr50;
	case 38: goto tr50;
	case 39: goto tr50;
	case 8: goto tr10;
	case 9: goto tr10;
	case 40: goto tr50;
	case 41: goto tr50;
	case 42: goto tr50;
	case 43: goto tr50;
	case 44: goto tr50;
	case 45: goto tr50;
	case 46: goto tr50;
	case 47: goto tr50;
	case 48: goto tr50;
	case 49: goto tr50;
	case 50: goto tr50;
	case 51: goto tr50;
	case 52: goto tr50;
	case 53: goto tr50;
	case 54: goto tr50;
	case 55: goto tr50;
	case 56: goto tr89;
	case 57: goto tr50;
	case 58: goto tr50;
	case 59: goto tr50;
	case 10: goto tr10;
	case 11: goto tr10;
	case 60: goto tr50;
	case 61: goto tr50;
	case 62: goto tr50;
	case 63: goto tr50;
	case 64: goto tr50;
	case 65: goto tr50;
	case 66: goto tr50;
	case 67: goto tr50;
	case 68: goto tr50;
	case 69: goto tr50;
	case 70: goto tr50;
	}
	}

	_out: {}
	}

    location->second = p - s;

    if (cs == 
0
) {
        // TODO(sandello): Handle lexer failures.
        return TToken::Failure;
    } else {
        return type;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NAst
} // namespace NQueryClient
} // namespace NYT

