#include "tiny-toml.h"

#include <cassert>

namespace TinyToml
{
    namespace
    {
        Cursor make_cursor(String8 input)
        {
            Cursor result{
                .input = input,
                .line = Line{ 1 }
            };
            return result;
        }

        bool cursor_exhausted(const Cursor& cursor)
        {
            return rep(cursor.index) >= cursor.input.size;
        }

        Index cursor_tell(const Cursor& cursor)
        {
            return cursor.index;
        }

        String8 cursor_snip(const Cursor& cursor, Index start, Index end)
        {
            return str8_substr(cursor.input, { .off = rep(start), .len = rep(end) - rep(start) });
        }

        char cursor_peek(const Cursor& cursor)
        {
            if (cursor_exhausted(cursor))
                return '\0';
            return cursor.input.str[rep(cursor.index)];
        }

        char cursor_take(Cursor* cursor)
        {
            if (cursor_exhausted(*cursor))
                return '\0';
            char c = cursor->input.str[rep(cursor->index)];
            cursor->index = extend(cursor->index);
            return c;
        }

        ParseMsg* push_parse_atom(Parser* parser, ParseMsg msg)
        {
            ParseMsg* node = Arena::push_array_no_zero<ParseMsg>(parser->msgs_arena, 1);
            *node = msg;
            node->next = nullptr;
            SLLQueuePush(parser->parse_atoms.first, parser->parse_atoms.last, node);
            ++parser->parse_atoms.count;
            return node;
        }

        ParseMsg* push_parse_msg(Parser* parser, ParseMsg msg)
        {
            ParseMsg* node = Arena::push_array_no_zero<ParseMsg>(parser->msgs_arena, 1);
            *node = msg;
            node->next = nullptr;
            SLLQueuePush(parser->parse_msgs.first, parser->parse_msgs.last, node);
            ++parser->parse_msgs.count;
            return node;
        }

        ParseMsg* push_err_msg(Parser* parser, ErrorInternal* err)
        {
            ParseMsg* node = Arena::push_array_no_zero<ParseMsg>(parser->msgs_arena, 1);
            node->error.err = err;
            node->kind = ParseMsg::Kind::Error;
            node->locus = err->locus;
            node->next = nullptr;
            SLLQueuePush(parser->err_msgs.first, parser->err_msgs.last, node);
            ++parser->err_msgs.count;
            return node;
        }

        ParseMsg* unexpected_eol_error(Parser* parser)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::UnexpectedEOL;
            return push_err_msg(parser, node);
        }

        ParseMsg* unknown_character_error(Parser* parser, char c)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::UnknownChar;
            node->unknown_char.c = c;
            return push_err_msg(parser, node);
        }

        ParseMsg* unexpected_error(Parser* parser, char expected, char actual)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::UnexpectedChar;
            node->unexpected_char.expected = expected;
            node->unexpected_char.actual = actual;
            return push_err_msg(parser, node);
        }

        ParseMsg* incomplete_crlf_error(Parser* parser)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::IncompleteCRLF;
            return push_err_msg(parser, node);
        }

        ParseMsg* unexpected_eof_error(Parser* parser)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::UnexpectedEOF;
            return push_err_msg(parser, node);
        }

        ParseMsg* invalid_escape_error(Parser* parser, Locus locus)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus;
            node->kind = ErrorInternal::Kind::InvalidEscape;
            return push_err_msg(parser, node);
        }

        ParseMsg* numeric_value_error(Parser* parser)
        {
            ErrorInternal* node = Arena::push_array<ErrorInternal>(parser->msgs_arena, 1);
            node->locus = locus(parser->cursor);
            node->kind = ErrorInternal::Kind::NumericValue;
            return push_err_msg(parser, node);
        }

        constexpr bool is_space(char c)
        {
            return c == ' ' or c == '\t';
        }

        constexpr bool is_lf(char c)
        {
            return c == '\n';
        }

        constexpr bool is_lower(char c)
        {
            return c >= 'a' and c <= 'z';
        }

        constexpr bool is_upper(char c)
        {
            return c >= 'A' and c <= 'Z';
        }

        constexpr bool is_alpha(char c)
        {
            return is_lower(c) or is_upper(c);
        }

        constexpr bool is_digit(char c)
        {
            return c >= '0' and c <= '9';
        }

        constexpr bool is_xdigit(char c)
        {
            return is_digit(c) or ('a' <= c and c <= 'f') or ('A' <= c and c <= 'F');
        }

        constexpr bool is_binary_digit(char c)
        {
            return c == '0' or c == '1';
        }

        constexpr bool is_octal_digit(char c)
        {
            return '0' <= c and c <= '7';
        }

        constexpr bool is_key_first(char c)
        {
            return c == '-' or c == '_' or is_alpha(c) or is_digit(c);
        }

        void process_lf(Parser* parser)
        {
            parser->cursor.beginning_of_line = parser->cursor.index;
            parser->cursor.line = extend(parser->cursor.line);
        }

        template <typename Pred>
        bool take_if(Parser* parser, Pred pred)
        {
            char c = cursor_peek(parser->cursor);
            if (not pred(c))
                return false;
            cursor_take(&parser->cursor);
            return true;
        }

        template <typename Pred>
        bool take_while(Parser* parser, Pred pred)
        {
            if (not take_if(parser, pred))
                return false;
            while (take_if(parser, pred));
            return true;
        }

        bool take_if(Parser* parser, char c)
        {
            if (cursor_peek(parser->cursor) == c)
            {
                cursor_take(&parser->cursor);
                return true;
            }
            return false;
        }

        bool peek_if(Parser* parser, char c)
        {
            return cursor_peek(parser->cursor) == c;
        }

        void eat_line(Parser* parser)
        {
            while (not cursor_exhausted(parser->cursor) and cursor_peek(parser->cursor) != '\n')
            {
                cursor_take(&parser->cursor);
            }
        }

        void take_until(Parser* parser, char c)
        {
            while (cursor_peek(parser->cursor) != c)
            {
                cursor_take(&parser->cursor);
                if (cursor_exhausted(parser->cursor))
                {
                    unexpected_eof_error(parser);
                    break;
                }
            }
        }

        void require(Parser* parser, char c)
        {
            if (not take_if(parser, c))
            {
                unexpected_error(parser, c, cursor_take(&parser->cursor));
            }
        }

        void require(Parser* parser, String8 literal)
        {
            for EachIndex(i, literal.size)
            {
                require(parser, literal.str[i]);
            }
        }

        void space(Parser* parser)
        {
            take_while(parser, is_space);
        }

        void lf(Parser* parser)
        {
            require(parser, '\n');
            process_lf(parser);
        }

        void lfs(Parser* parser)
        {
            while (peek_if(parser, '\n'))
            {
                lf(parser);
            }
        }

        void crlf(Parser* parser)
        {
            require(parser, '\r');
            if (not take_if(parser, '\n'))
            {
                incomplete_crlf_error(parser);
            }
            else
            {
                process_lf(parser);
            }
        }

        void crlfs(Parser* parser)
        {
            while (peek_if(parser, '\r'))
            {
                crlf(parser);
            }
        }

        void space_or_newline(Parser* parser)
        {
            while (char c = cursor_peek(parser->cursor))
            {
                switch (c)
                {
                case ' ':  space(parser); break;
                case '\t': space(parser); break;
                case '\n': lfs(parser);   break;
                case '\r': crlfs(parser); break;
                default:                  return;
                }
            }
        }

        void comment(Parser* parser)
        {
            eat_line(parser);
        }

        ParseMsg* process_unexpected_character(Parser* parser, char c)
        {
            switch (c)
            {
            case '\n':
                process_lf(parser);
                return unexpected_eol_error(parser);
            case '\r':
                if (not take_if(parser, '\n'))
                {
                    incomplete_crlf_error(parser);
                    return unknown_character_error(parser, c);
                }
                process_lf(parser);
                return unexpected_eol_error(parser);
            default:
                return unknown_character_error(parser, c);
            }
        }

        void require_newline(Parser* parser)
        {
            switch (cursor_peek(parser->cursor))
            {
            case '\n': lfs(parser);   return;
            case '\r': crlfs(parser); return;
            default:                  break;
            }
            process_unexpected_character(parser, cursor_take(&parser->cursor));
        }

        ParseMsg* unquoted_key(Parser* parser)
        {
            if (not is_key_first(cursor_peek(parser->cursor)))
                return process_unexpected_character(parser, cursor_take(&parser->cursor));
            Locus l = locus(parser->cursor);
            Index start = cursor_tell(parser->cursor);
            take_while(parser, is_key_first);
            String8 text = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::BareKey;
            msg.bare_key.key = text;
            msg.locus = l;
            return push_parse_atom(parser, msg);
        }

        ParseMsg* simple_key(Parser* parser)
        {
            // TODO: Support quoted keys as well, but our config does not need them yet.
            return unquoted_key(parser);
        }

        ParseMsg* key(Parser* parser)
        {
            ParseMsg* k = simple_key(parser);
            if (peek_if(parser, '.'))
            {
                ParseMsg msg{};
                msg.kind = ParseMsg::Kind::DottedKey;
                msg.locus = k->locus;
                msg.dotted_key = {};
                cursor_take(&parser->cursor); // consume '.'.
                do
                {
                    DottedKeyEntry* node = Arena::push_array<DottedKeyEntry>(parser->msgs_arena, 1);
                    node->part = k;
                    SLLQueuePush(msg.dotted_key.first, msg.dotted_key.last, node);
                    ++msg.dotted_key.count;
                    k = simple_key(parser);
                } while (take_if(parser, '.'));
                // Add the last key.
                DottedKeyEntry* node = Arena::push_array<DottedKeyEntry>(parser->msgs_arena, 1);
                node->part = k;
                SLLQueuePush(msg.dotted_key.first, msg.dotted_key.last, node);
                ++msg.dotted_key.count;
                return push_parse_atom(parser, msg);
            }
            return k;
        }

        ParseMsg* boolean_true(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            require(parser, str8_mut(str8_literal("true")));
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::True;
            msg.locus = l;
            return push_parse_atom(parser, msg);
        }

        ParseMsg* boolean_false(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            require(parser, str8_mut(str8_literal("false")));
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::False;
            msg.locus = l;
            return push_parse_atom(parser, msg);
        }

        uint32_t hex(Parser* parser)
        {
            char c = cursor_take(&parser->cursor);
            if (c >= '0' and c <= '9')
                return c - '0';
            if (c >= 'a' and c <= 'f')
                return c - 'W';
            if (c >= 'A' and c <= 'F')
                return c - '7';
            process_unexpected_character(parser, c);
            return 0xFFFF;
        }

        uint64_t hex_escaped(Parser* parser, int bytes)
        {
            uint64_t value = 0;
            for (auto i = 0; i < bytes; ++i)
                value = (value << 8) | hex(parser);
            return value;
        }

        void unicode_escape(Parser* parser, Locus locus, uint64_t v)
        {
            if (v >= 0x0 and v <= 0xD7FF)
                return;
            if (v >= 0xE000 and v <= 0x10FFFF)
                return;
            invalid_escape_error(parser, locus);
        }

        void long_unicode_escape_sequence(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            unicode_escape(parser, l, hex_escaped(parser, 8));
        }

        void short_unicode_escape_sequence(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            unicode_escape(parser, l, hex_escaped(parser, 4));
        }

        bool valid_escape(char c)
        {
            constexpr String8View cs = str8_literal("btnfr\"\\");
            for EachIndex(i, cs.size)
            {
                if (c == cs.str[i])
                    return true;
            }
            return false;
        }

        void escape_sequence(Parser* parser)
        {
            char c = cursor_take(&parser->cursor);
            if (c == 'u')
                return short_unicode_escape_sequence(parser);
            if (c == 'U')
                return long_unicode_escape_sequence(parser);
            if (not valid_escape(c))
            {
                invalid_escape_error(parser, locus(parser->cursor));
            }
        }

        void basic_string_element(Parser* parser)
        {
            if (take_if(parser, '\\'))
                return escape_sequence(parser);
            cursor_take(&parser->cursor);
        }

        ParseMsg* basic_string(Parser* parser)
        {
            require(parser, '"');
            Index start = cursor_tell(parser->cursor);
            Locus start_locus = locus(parser->cursor);
            while (not peek_if(parser, '"'))
                basic_string_element(parser);
            String8 str = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
            require(parser, '"');
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::String;
            msg.locus = start_locus;
            msg.string.str = str;
            return push_parse_atom(parser, msg);
        }

        ParseMsg* literal_string(Parser* parser)
        {
            require(parser, '\'');
            Index start = cursor_tell(parser->cursor);
            Locus start_locus = locus(parser->cursor);
            take_until(parser, '\'');
            String8 str = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
            require(parser, '\'');
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::StringLiteral;
            msg.locus = start_locus;
            msg.string.str = str;
            return push_parse_atom(parser, msg);
        }

        template <typename CharClassTest>
        void scan_int(Parser* parser, CharClassTest char_class)
        {
            // An integer is a digit sequence: NonZeroDigit { _ | Digit }
            // with the added constraint that the last char is a digit and
            // no '_' follows another '_'.
            bool usable = true;
            if (not char_class(cursor_take(&parser->cursor)))
            {
                numeric_value_error(parser);
                usable = false;
            }

            if (usable)
            {
                while (char_class(cursor_peek(parser->cursor)) or cursor_peek(parser->cursor) == '_')
                {
                    if (take_if(parser, '_'))
                    {
                        // A digit must follow.
                        if (not char_class(cursor_peek(parser->cursor)))
                        {
                            numeric_value_error(parser);
                        }
                    }
                    take_if(parser, char_class);
                }
            }
        }

        void scan_oct_int(Parser* parser)
        {
            cursor_take(&parser->cursor); // Consume 'o'.
            scan_int(parser, is_octal_digit);
        }

        void scan_hex_int(Parser* parser)
        {
            cursor_take(&parser->cursor); // Consume 'x'.
            scan_int(parser, is_xdigit);
        }

        void scan_bin_int(Parser* parser)
        {
            cursor_take(&parser->cursor); // Consume 'b'.
            scan_int(parser, is_binary_digit);
        }

        void scan_decimal_int(Parser* parser)
        {
            scan_int(parser, is_digit);
        }

        void scan_float_exp(Parser* parser)
        {
            // e, E have already been consumed.
            if (cursor_peek(parser->cursor) == '+' or cursor_peek(parser->cursor) == '-')
            {
                cursor_take(&parser->cursor);
            }
            scan_decimal_int(parser);
        }

        void scan_float(Parser* parser)
        {
            char c = cursor_take(&parser->cursor);
            switch (c)
            {
            case '.':
                scan_decimal_int(parser);
                if (cursor_peek(parser->cursor) == 'e' or cursor_peek(parser->cursor) == 'E')
                {
                    cursor_take(&parser->cursor);
                    scan_float_exp(parser);
                }
                break;
            case 'e':
            case 'E':
                scan_float_exp(parser);
                break;
            default:
                numeric_value_error(parser);
                break;
            }
        }

        void scan_hex_float_exponent(Parser* parser, HexFloatData* hex_flt)
        {
            cursor_take(&parser->cursor); // Consume 'p'.
            switch (cursor_peek(parser->cursor))
            {
            case '-':
                hex_flt->pieces.neg_expon = 1;
                [[fallthrough]];
            case '+':
                cursor_take(&parser->cursor);
                [[fallthrough]];
            default:
                if (is_digit(cursor_peek(parser->cursor)))
                {
                    auto start = cursor_tell(parser->cursor);
                    scan_int(parser, is_digit);
                    hex_flt->pieces.exponent.start = static_cast<uint8_t>(cursor_snip(parser->cursor, start, start).str - hex_flt->base);
                    hex_flt->pieces.exponent.length = static_cast<uint8_t>(rep(cursor_tell(parser->cursor)) - rep(start));
                }
                else
                {
                    process_unexpected_character(parser, cursor_take(&parser->cursor));
                }
                break;
            }
        }

        enum class Sign { None, Positive, Negative };

        HexFloatData scan_hex_float(Parser* parser, Index start, Sign sign)
        {
            HexFloatData hex_flt{};
            hex_flt.pieces.neg = sign == Sign::Negative;
            // Rebase at start.
            hex_flt.base = cursor_snip(parser->cursor, start, start).str;
            hex_flt.pieces.whole.start = 0;
            hex_flt.pieces.whole.length = static_cast<uint8_t>(rep(cursor_tell(parser->cursor)) - rep(start));
            // The fractional part is optional.
            if (take_if(parser, '.') and is_xdigit(cursor_peek(parser->cursor)))
            {
                start = cursor_tell(parser->cursor);
                scan_int(parser, is_xdigit);
                hex_flt.pieces.fractional.start = static_cast<uint8_t>(cursor_snip(parser->cursor, start, start).str - hex_flt.base);
                hex_flt.pieces.fractional.length = static_cast<uint8_t>(rep(cursor_tell(parser->cursor)) - rep(start));
            }
            switch (cursor_peek(parser->cursor))
            {
            case 'p':
            case 'P':
                scan_hex_float_exponent(parser, &hex_flt);
                break;
            default:
                numeric_value_error(parser);
                break;
            }
            return hex_flt;
        }

        void scan_date(Parser* parser)
        {
            // NYI.
            numeric_value_error(parser);
        }

        ParseMsg* zero_prefixed_number(Parser* parser, Index start, Sign sign)
        {
            cursor_take(&parser->cursor); // Consume the '0'.
            // Note: this can be hex/bin/oct integral or float.
            char c = cursor_peek(parser->cursor);
            ParseMsg::Kind kind = ParseMsg::Kind::Decimal;
            switch (c)
            {
            case 'x':
            case 'X':
                // The start will be after we've consumed the 'x'.
                start = extend(cursor_tell(parser->cursor));
                scan_hex_int(parser);
                // Try to parse as a hex float.
                if (peek_if(parser, '.') or peek_if(parser, 'p'))
                {
                    ParseMsg msg{};
                    msg.kind = ParseMsg::Kind::HexFloat;
                    msg.locus = locus(parser->cursor);
                    msg.hex_flt = scan_hex_float(parser, start, sign);
                    return push_parse_atom(parser, msg);
                }
                else
                {
                    if (sign != Sign::None)
                    {
                        numeric_value_error(parser);
                    }
                    kind = ParseMsg::Kind::DecimalHex;
                }
                break;
            case 'o':
            case 'O':
                if (sign != Sign::None)
                {
                    numeric_value_error(parser);
                }
                // The start will be after we've consumed the 'o'.
                start = extend(cursor_tell(parser->cursor));
                scan_oct_int(parser);
                kind = ParseMsg::Kind::DecimalOct;
                break;
            case 'b':
            case 'B':
                if (sign != Sign::None)
                {
                    numeric_value_error(parser);
                }
                // The start will be after we've consumed the 'b'.
                start = extend(cursor_tell(parser->cursor));
                scan_bin_int(parser);
                kind = ParseMsg::Kind::DecimalBin;
                break;
            case '.':
                {
                    scan_float(parser);
                    ParseMsg msg{};
                    msg.kind = ParseMsg::Kind::Float;
                    msg.locus = locus(parser->cursor);
                    msg.flt.number = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
                    return push_parse_atom(parser, msg);
                }
            default:
                // Parse as octal (extension).
                if (is_digit(c))
                {
                    if (sign != Sign::None)
                    {
                        numeric_value_error(parser);
                    }
                    // Start the number now.
                    start = cursor_tell(parser->cursor);
                    scan_int(parser, is_octal_digit);
                    kind = ParseMsg::Kind::DecimalOct;
                }
                else
                {
                    // It's a singular 0.
                }
                break;
            }
            ParseMsg msg{};
            msg.kind = kind;
            msg.locus = locus(parser->cursor);
            msg.decimal.number = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
            return push_parse_atom(parser, msg);
        }

        ParseMsg* number(Parser* parser, Index start)
        {
            // Decimal, float, or date.
            scan_decimal_int(parser);
            switch (cursor_peek(parser->cursor))
            {
            case '.':
            case 'e':
            case 'E':
                {
                    scan_float(parser);
                    ParseMsg msg{};
                    msg.kind = ParseMsg::Kind::Float;
                    msg.locus = locus(parser->cursor);
                    msg.flt.number = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
                    return push_parse_atom(parser, msg);
                }
            case '-':
                scan_date(parser);
                break;
            }
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::Decimal;
            msg.locus = locus(parser->cursor);
            msg.decimal.number = cursor_snip(parser->cursor, start, cursor_tell(parser->cursor));
            return push_parse_atom(parser, msg);
        }

        ParseMsg* signed_number(Parser* parser, Index start, Sign sign)
        {
            cursor_take(&parser->cursor); // Consume sign.
            if (cursor_peek(parser->cursor) == '0')
                return zero_prefixed_number(parser, start, sign);
            return number(parser, start);
        }

        // We need array builder for 'val'.
        ParseMsg* array(Parser* parser);

        ParseMsg* val(Parser* parser)
        {
            switch (cursor_peek(parser->cursor))
            {
            case 't':
                return boolean_true(parser);
            case 'f':
                return boolean_false(parser);
            case '[':
                return array(parser);
            case '"':
                return basic_string(parser);
            case '\'':
                return literal_string(parser);
            case '-':
                return signed_number(parser, cursor_tell(parser->cursor), Sign::Negative);
            case '+':
                return signed_number(parser, cursor_tell(parser->cursor), Sign::Positive);
            case '0':
                return zero_prefixed_number(parser, cursor_tell(parser->cursor), Sign::None);
            default:
                if (is_digit(cursor_peek(parser->cursor)))
                    return number(parser, cursor_tell(parser->cursor));
            }
            return process_unexpected_character(parser, cursor_take(&parser->cursor));
        }

        ParseMsg* array(Parser* parser)
        {
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::Array;
            msg.array = {};
            msg.locus = locus(parser->cursor);
            require(parser, '[');
            while (not take_if(parser, ']'))
            {
                space_or_newline(parser);

                ParseMsg* v = val(parser);
                ArrayValue* arr_val = Arena::push_array<ArrayValue>(parser->msgs_arena, 1);
                arr_val->data = v;
                SLLQueuePush(msg.array.first, msg.array.last, arr_val);
                ++msg.array.count;

                space_or_newline(parser);
                if (cursor_peek(parser->cursor) != ']')
                {
                    require(parser, ',');
                }
            }
            return push_parse_atom(parser, msg);
        }

        ParseMsg* pair(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            ParseMsg* k = key(parser);
            space(parser);
            require(parser, '=');
            space(parser);
            ParseMsg* v = val(parser);
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::Pair;
            msg.locus = l;
            msg.pair.key = k;
            msg.pair.val = v;
            return push_parse_msg(parser, msg);
        }

        ParseMsg* table(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            ParseMsg* k = key(parser);
            require(parser, ']');
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::Table;
            msg.table.key = k;
            msg.locus = l;
            return push_parse_msg(parser, msg);
        }

        ParseMsg* array_of_table(Parser* parser)
        {
            Locus l = locus(parser->cursor);
            ParseMsg* k = key(parser);
            require(parser, ']');
            require(parser, ']');
            ParseMsg msg{};
            msg.kind = ParseMsg::Kind::ArrayOfTable;
            msg.array_of_table.key = k;
            msg.locus = l;
            return push_parse_msg(parser, msg);
        }

        ParseMsg* table_or_array_of_table(Parser* parser)
        {
            cursor_take(&parser->cursor); // Consume '['.
            if (take_if(parser, '['))
                return array_of_table(parser);
            return table(parser);
        }

        ParseMsg* expression(Parser* parser)
        {
            switch (cursor_peek(parser->cursor))
            {
            case '[':
                return table_or_array_of_table(parser);
            default:
                return pair(parser);
            }
        }

        void expression_comment(Parser* parser)
        {
            if (take_if(parser, '#'))
            {
                comment(parser);
                return;
            }
            expression(parser);
            space(parser);
            if (take_if(parser, '#'))
                comment(parser);
        }

        void parse_inner(Parser* parser)
        {
            do
            {
                require_newline(parser);
                space_or_newline(parser);
                if (not cursor_exhausted(parser->cursor))
                {
                    expression_comment(parser);
                    // This is the very end of the production.
                    if (cursor_exhausted(parser->cursor))
                        break;
                }
                else
                {
                    break;
                }
            } while(true);
        }
    } // namespace [anon]

    // Parser creation.
    Parser make_parser(Arena::Arena* msgs_arena, String8 input)
    {
        Parser parser{};
        parser.cursor = make_cursor(input);
        parser.msgs_arena = msgs_arena;
        return parser;
    }

    // Parser entry.
    void parser_parse(Parser* parser)
    {
        // We're going to perform the first expression pull, and then dispatch to parse_inner
        // which is responsible for expecting newlines preceding any new expression.
        space_or_newline(parser);
        if (not cursor_exhausted(parser->cursor))
        {
            expression_comment(parser);
        }

        if (not cursor_exhausted(parser->cursor))
        {
            // Fall into the *(newline expression) production.
            parse_inner(parser);
        }
        // Buffer must be exhausted.
        require(parser, '\0');
        ParseMsg msg{};
        msg.kind = ParseMsg::Kind::Fin;
        msg.locus = locus(parser->cursor);
        push_parse_msg(parser, msg);
    }

    // Queries.
    bool has_errors(const Parser& parser)
    {
        return parser.err_msgs.count != 0;
    }

    // Helpers.
    bool dotted_key_matches(ParseMsg* msg, String8 key)
    {
        if (msg->kind != ParseMsg::Kind::DottedKey)
            return false;
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        SplitStringsInput in{};
        in.in = key;
        in.seps = str8_mut(str8_literal("."));
        String8List split_result{};
        split_strings(scratch.arena, &split_result, in);
        bool good = split_result.node_count == msg->dotted_key.count;
        if (good)
        {
            String8Node* first_k = split_result.first;
            for EachNode(n, msg->dotted_key.first)
            {
                if (not bare_key_matches(n->part, first_k->string))
                {
                    good = false;
                    break;
                }
                first_k = first_k->next;
            }
        }
        Arena::scratch_end(scratch);
        return good;
    }

    bool bare_key_matches(ParseMsg* msg, String8 key)
    {
        if (msg->kind != ParseMsg::Kind::BareKey)
            return false;
        return str8_match_exact(msg->bare_key.key, key);
    }

    bool any_string(ParseMsg* msg)
    {
        switch (msg->kind)
        {
        case ParseMsg::Kind::String:
        case ParseMsg::Kind::StringLiteral:
            return true;
        }
        return false;
    }

    bool any_integral(ParseMsg* msg)
    {
        switch (msg->kind)
        {
        case ParseMsg::Kind::Decimal:
        case ParseMsg::Kind::DecimalHex:
        case ParseMsg::Kind::DecimalOct:
        case ParseMsg::Kind::DecimalBin:
            return true;
        }
        return false;
    }

    bool any_boolean(ParseMsg* msg)
    {
        switch (msg->kind)
        {
        case ParseMsg::Kind::True:
        case ParseMsg::Kind::False:
            return true;
        }
        return false;
    }

    String8 key_val(Arena::Arena* arena, ParseMsg* msg)
    {
        String8 result = str8_empty;
        switch (msg->kind)
        {
        case ParseMsg::Kind::BareKey:
            result = msg->bare_key.key;
            break;
        case ParseMsg::Kind::DottedKey:
            {
                auto scratch = Arena::scratch_begin({ &arena, 1 });
                String8List str_lst{};
                for EachNode(n, msg->dotted_key.first)
                {
                    str8_list_push(scratch.arena, &str_lst, str8_copy(scratch.arena, key_val(scratch.arena, n->part)));
                    if (n->next != nullptr)
                    {
                        str8_list_push(scratch.arena, &str_lst, str8_mut(str8_literal(".")));
                    }
                }
                result = str8_list_join(arena, str_lst);
                Arena::scratch_end(scratch);
            }
            break;
        }
        return result;
    }

    String8 string_val(ParseMsg* msg)
    {
        assert(any_string(msg));
        return msg->string.str;
    }

    uint64_t integral_val(ParseMsg* msg)
    {
        assert(any_integral(msg));
        switch (msg->kind)
        {
        case ParseMsg::Kind::Decimal:
            return u64_from_str8(msg->decimal.number, 10);
        case ParseMsg::Kind::DecimalHex:
            return u64_from_str8(msg->decimal.number, 16);
        case ParseMsg::Kind::DecimalOct:
            return u64_from_str8(msg->decimal.number, 8);
        case ParseMsg::Kind::DecimalBin:
            return u64_from_str8(msg->decimal.number, 2);
        }
        return 0;
    }

    int64_t signed_integral_val(ParseMsg* msg)
    {
        assert(any_integral(msg));
        switch (msg->kind)
        {
        case ParseMsg::Kind::Decimal:
            return s64_from_str8(msg->decimal.number, 10);
        case ParseMsg::Kind::DecimalHex:
            return s64_from_str8(msg->decimal.number, 16);
        case ParseMsg::Kind::DecimalOct:
            return s64_from_str8(msg->decimal.number, 8);
        case ParseMsg::Kind::DecimalBin:
            return s64_from_str8(msg->decimal.number, 2);
        }
        return 0;
    }

    bool boolean_val(ParseMsg* msg)
    {
        assert(any_boolean(msg));
        return msg->kind == ParseMsg::Kind::True;
    }

    Locus locus(const Cursor& cursor)
    {
        Locus locus{
            .line = cursor.line,
            .col = Column{ rep(cursor.index) - rep(cursor.beginning_of_line) + 1 }
        };
        return locus;
    }

    ENABLE_UNHANDLED_CASE_WARNING();
    String8View err_kind_string(ErrorInternal::Kind k)
    {
        switch (k)
        {
        case ErrorInternal::Kind::UnknownChar:
            return str8_literal("UnknownChar");
        case ErrorInternal::Kind::UnexpectedEOL:
            return str8_literal("UnexpectedEOL");
        case ErrorInternal::Kind::UnexpectedChar:
            return str8_literal("UnexpectedChar");
        case ErrorInternal::Kind::IncompleteCRLF:
            return str8_literal("IncompleteCRLF");
        case ErrorInternal::Kind::UnexpectedEOF:
            return str8_literal("UnexpectedEOF");
        case ErrorInternal::Kind::InvalidEscape:
            return str8_literal("InvalidEscape");
        case ErrorInternal::Kind::NumericValue:
            return str8_literal("NumericValue");
        }
        return str8_literal("");
    }

    String8View msg_kind_string(ParseMsg::Kind k)
    {
        switch (k)
        {
        case ParseMsg::Kind::Error:
            return str8_literal("Error");
        case ParseMsg::Kind::Fin:
            return str8_literal("Fin");
        case ParseMsg::Kind::Table:
            return str8_literal("Table");
        case ParseMsg::Kind::ArrayOfTable:
            return str8_literal("ArrayOfTable");
        case ParseMsg::Kind::DottedKey:
            return str8_literal("DottedKey");
        case ParseMsg::Kind::BareKey:
            return str8_literal("BareKey");
        case ParseMsg::Kind::Pair:
            return str8_literal("Pair");
        case ParseMsg::Kind::Float:
            return str8_literal("Float");
        case ParseMsg::Kind::HexFloat:
            return str8_literal("HexFloat");
        case ParseMsg::Kind::Decimal:
            return str8_literal("Decimal");
        case ParseMsg::Kind::DecimalHex:
            return str8_literal("DecimalHex");
        case ParseMsg::Kind::DecimalOct:
            return str8_literal("DecimalOct");
        case ParseMsg::Kind::DecimalBin:
            return str8_literal("DecimalBin");
        case ParseMsg::Kind::True:
            return str8_literal("True");
        case ParseMsg::Kind::False:
            return str8_literal("False");
        case ParseMsg::Kind::Array:
            return str8_literal("Array");
        case ParseMsg::Kind::String:
            return str8_literal("String");
        case ParseMsg::Kind::StringLiteral:
            return str8_literal("StringLiteral");
        case ParseMsg::Kind::Count:
            break;
        }
        return str8_literal("");
    }
    DISABLE_UNHANDLED_CASE_WARNING();

    String8 escape_string(Arena::Arena* arena, String8 unescaped)
    {
        struct EscapeChar
        {
            char escape;
            char unescaped;
        };
        constexpr EscapeChar escapes[] =
        {
            { .escape = '\b', .unescaped = 'b' },
            { .escape = '\t', .unescaped = 't' },
            { .escape = '\n', .unescaped = 'n' },
            { .escape = '\f', .unescaped = 'f' },
            { .escape = '\r', .unescaped = 'r' },
            { .escape = '"',  .unescaped = '"' },
            { .escape = '\\', .unescaped = '\\' },
        };
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List serial{};
        str8_serial_begin(scratch.arena, &serial);
        for EachIndex(i, unescaped.size)
        {
            char escape = 0;
            for (EscapeChar e : escapes)
            {
                if (unescaped.str[i] == e.escape)
                {
                    escape = e.unescaped;
                    break;
                }
            }

            if (escape)
            {
                str8_serial_push_char(scratch.arena, &serial, '\\');
                str8_serial_push_char(scratch.arena, &serial, escape);
            }
            else
            {
                str8_serial_push_char(scratch.arena, &serial, unescaped.str[i]);
            }
        }
        String8 result = str8_serial_end(arena, serial);
        Arena::scratch_end(scratch);
        return result;
    }

    String8 process_escapes(Arena::Arena* arena, String8 escaped)
    {
        struct EscapeChar
        {
            char escape;
            char unescaped;
        };
        constexpr EscapeChar escapes[] =
        {
            { .escape = '\b', .unescaped = 'b' },
            { .escape = '\t', .unescaped = 't' },
            { .escape = '\n', .unescaped = 'n' },
            { .escape = '\f', .unescaped = 'f' },
            { .escape = '\r', .unescaped = 'r' },
            { .escape = '"',  .unescaped = '"' },
            { .escape = '\\', .unescaped = '\\' },
        };
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List serial{};
        str8_serial_begin(scratch.arena, &serial);
        for EachIndex(i, escaped.size)
        {
            char escape = 0;
            if (escaped.str[i] == '\\')
            {
                // Check if the next char is an escape.
                char next_c = 0;
                if (i + 1 < escaped.size)
                {
                    next_c = escaped.str[i + 1];
                }
                for (EscapeChar e : escapes)
                {
                    if (next_c == e.unescaped)
                    {
                        escape = e.escape;
                        break;
                    }
                }
            }

            if (escape)
            {
                str8_serial_push_char(scratch.arena, &serial, escape);
                // Increment 'i' so we skip over the char we just escaped as well.
                ++i;
            }
            else
            {
                str8_serial_push_char(scratch.arena, &serial, escaped.str[i]);
            }
        }
        String8 result = str8_serial_end(arena, serial);
        Arena::scratch_end(scratch);
        return result;
    }
} // namespace TinyToml