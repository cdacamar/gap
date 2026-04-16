#pragma once

#include "arena.h"
#include "gap-strings.h"
#include "types.h"

namespace TinyToml
{
    enum class Index : uint64_t { };
    enum class Line : uint64_t { };

    struct Cursor
    {
        String8 input;
        Index index;
        Index beginning_of_line;
        Line line;
    };

    enum class Column : uint64_t { };

    struct Locus
    {
        Line line;
        Column col;
    };

    struct ParseMsg;

    struct UnknownCharErr
    {
        char c;
    };

    struct UnexpectedCharErr
    {
        char expected;
        char actual;
    };

    struct ErrorInternal
    {
        enum class Kind
        {
            UnknownChar,
            UnexpectedChar,
            UnexpectedEOL,
            IncompleteCRLF,
            UnexpectedEOF,
            InvalidEscape,
            NumericValue,
        };

        Locus locus;
        Kind kind;
        union
        {
            UnknownCharErr unknown_char;
            UnexpectedCharErr unexpected_char;
        };
    };

    struct ErrorData
    {
        // Indirect it to keep the allocation sizes smaller since errors are
        // 1: Rare and,
        // 2: Arbitrarily large to be descriptive.
        ErrorInternal* err;
    };

    struct TableData
    {
        ParseMsg* key;
    };

    struct ArrayOfTableData
    {
        ParseMsg* key;
    };

    struct DottedKeyEntry
    {
        DottedKeyEntry* next;
        ParseMsg* part;
    };

    struct DottedKeyData
    {
        DottedKeyEntry* first;
        DottedKeyEntry* last;
        uint64_t count;
    };

    struct BareKeyData
    {
        String8 key;
    };

    struct PairData
    {
        ParseMsg* key;
        ParseMsg* val;
    };

    struct FloatData
    {
        String8 number;
    };

    struct TinyString8Slice
    {
        uint8_t start;
        uint8_t length;
    };

    struct HexFloatPieces
    {
        TinyString8Slice whole;
        TinyString8Slice fractional;
        TinyString8Slice exponent;
        uint8_t neg;
        uint8_t neg_expon;
    };

    struct HexFloatData
    {
        char* base;
        union
        {
            HexFloatPieces pieces;
            uint8_t arr[16];
        };
    };

    struct DecimalData
    {
        String8 number;
    };

    struct ArrayValue
    {
        ArrayValue* next;
        ParseMsg* data;
    };

    struct ArrayData
    {
        ArrayValue* first;
        ArrayValue* last;
        uint64_t count;
    };

    struct StringData
    {
        String8 str;
    };

    struct ParseMsg
    {
        enum class Kind
        {
            Error,
            Fin,
            Table,
            ArrayOfTable,
            DottedKey,
            BareKey,
            Pair,
            Float,
            HexFloat,
            Decimal,
            DecimalHex,
            DecimalOct,
            DecimalBin,
            True,
            False,
            Array,
            String,
            StringLiteral,
            Count
        };

        ParseMsg* next;
        Locus locus;
        Kind kind;
        union
        {
            ErrorData              error;
            TableData              table;
            ArrayOfTableData       array_of_table;
            DottedKeyData          dotted_key;
            BareKeyData            bare_key;
            PairData               pair;
            FloatData              flt;
            HexFloatData           hex_flt;
            DecimalData            decimal;
            ArrayData              array;
            StringData             string;
        };
    };

    static_assert(sizeof(ParseMsg) == sizeof(void*) * 7);

    struct ParseMsgList
    {
        ParseMsg* first;
        ParseMsg* last;
        uint64_t count;
    };

    struct Parser
    {
        Cursor cursor;
        Arena::Arena* msgs_arena;
        // For intermediate results (e.g. values or keys).
        ParseMsgList parse_atoms;
        // For complete expressions.
        ParseMsgList parse_msgs;
        ParseMsgList err_msgs;
    };

    // Parser creation.
    Parser make_parser(Arena::Arena* msgs_arena, String8 input);

    // Parser entry.
    void parser_parse(Parser* parser);

    // Queries.
    bool has_errors(const Parser& parser);

    // Helpers.
    bool dotted_key_matches(ParseMsg* msg, String8 key);
    bool bare_key_matches(ParseMsg* msg, String8 key);
    bool any_string(ParseMsg* msg);
    bool any_integral(ParseMsg* msg);
    bool any_boolean(ParseMsg* msg);
    String8 key_val(Arena::Arena* arena, ParseMsg* msg);
    String8 string_val(ParseMsg* msg);
    uint64_t integral_val(ParseMsg* msg);
    int64_t signed_integral_val(ParseMsg* msg);
    bool boolean_val(ParseMsg* msg);
    Locus locus(const Cursor& cursor);
    String8View err_kind_string(ErrorInternal::Kind k);
    String8View msg_kind_string(ParseMsg::Kind k);
    String8 escape_string(Arena::Arena* arena, String8 unescaped);
    String8 process_escapes(Arena::Arena* arena, String8 escaped);
} // namespace TinyToml