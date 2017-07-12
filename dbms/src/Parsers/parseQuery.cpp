#include <Parsers/parseQuery.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/Lexer.h>
#include <Parsers/TokenIterator.h>
#include <Common/StringUtils.h>
#include <Common/typeid_cast.h>
#include <IO/WriteHelpers.h>
#include <IO/Operators.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
}


/** From position in (possible multiline) query, get line number and column number in line.
  * Used in syntax error message.
  */
static std::pair<size_t, size_t> getLineAndCol(const char * begin, const char * pos)
{
    size_t line = 0;

    const char * nl;
    while (nullptr != (nl = reinterpret_cast<const char *>(memchr(begin, '\n', pos - begin))))
    {
        ++line;
        begin = nl + 1;
    }

    /// Lines numbered from 1.
    return { line + 1, pos - begin + 1 };
}


static std::string getSyntaxErrorMessage(
    const char * begin,
    const char * end,
    const char * max_parsed_pos,
    Expected expected,
    bool hilite,
    const std::string & description)
{
    String message;

    {
        WriteBufferFromString out(message);

        out << "Syntax error";

        if (!description.empty())
            out << " (" << description << ")";

        if (max_parsed_pos == end || *max_parsed_pos == ';')
        {
            out << ": failed at end of query.\n";

            if (expected && *expected && *expected != '.')
                out << "Expected " << expected;
        }
        else
        {
            out << ": failed at position " << (max_parsed_pos - begin + 1);

            /// If query is multiline.
            const char * nl = reinterpret_cast<const char *>(memchr(begin, '\n', end - begin));
            if (nullptr != nl && nl + 1 != end)
            {
                size_t line = 0;
                size_t col = 0;
                std::tie(line, col) = getLineAndCol(begin, max_parsed_pos);

                out << " (line " << line << ", col " << col << ")";
            }

            /// Hilite place of syntax error.
            if (hilite)
            {
                out << ":\n\n";
                out.write(begin, max_parsed_pos - begin);

                size_t bytes_to_hilite = 1;
                while (max_parsed_pos + bytes_to_hilite < end
                    && static_cast<unsigned char>(max_parsed_pos[bytes_to_hilite]) >= 0x80    /// UTF-8
                    && static_cast<unsigned char>(max_parsed_pos[bytes_to_hilite]) <= 0xBF)
                    ++bytes_to_hilite;

                out << "\033[41;1m" << std::string(max_parsed_pos, bytes_to_hilite) << "\033[0m";        /// Bright red background.
                out.write(max_parsed_pos + bytes_to_hilite, end - max_parsed_pos - bytes_to_hilite);
                out << "\n\n";

                if (expected && *expected && *expected != '.')
                    out << "Expected " << expected;
            }
            else
            {
                out << ": " << std::string(max_parsed_pos, std::min(SHOW_CHARS_ON_SYNTAX_ERROR, end - max_parsed_pos));

                if (expected && *expected && *expected != '.')
                    out << ", expected " << expected;
            }
        }
    }

    return message;
}


ASTPtr tryParseQuery(
    IParser & parser,
    const char * & pos,
    const char * end,
    std::string & out_error_message,
    bool hilite,
    const std::string & description,
    bool allow_multi_statements)
{
    Tokens tokens(pos, end);
    TokenIterator token_iterator(tokens);

    if (token_iterator->type == TokenType::EndOfStream
        || token_iterator->type == TokenType::Semicolon)
    {
        out_error_message = "Empty query";
        return nullptr;
    }

    Expected expected = "";
    const char * begin = pos;

    ASTPtr res;
    bool parse_res = parser.parse(token_iterator, res, expected);
    const char * max_parsed_pos = token_iterator.max().begin;

    /// Lexical error
    if (!parse_res && token_iterator->type > TokenType::EndOfStream)
    {
        expected = "any valid token";
        out_error_message = getSyntaxErrorMessage(begin, end, max_parsed_pos, expected, hilite, description);
        return nullptr;
    }

    /// Excessive input after query. Parsed query must end with end of data or semicolon or data for INSERT.
    ASTInsertQuery * insert = nullptr;
    if (parse_res)
        insert = typeid_cast<ASTInsertQuery *>(res.get());

    if (parse_res
        && token_iterator->type != TokenType::EndOfStream
        && token_iterator->type != TokenType::Semicolon
        && !(insert && insert->data))
    {
        expected = "end of query";
        out_error_message = getSyntaxErrorMessage(begin, end, max_parsed_pos, expected, hilite, description);
        return nullptr;
    }

    while (token_iterator->type == TokenType::Semicolon)
        ++token_iterator;

    /// If multi-statements are not allowed, then after semicolon, there must be no non-space characters.
    if (parse_res && !allow_multi_statements
        && token_iterator->type != TokenType::EndOfStream
        && !(insert && insert->data))
    {
        out_error_message = getSyntaxErrorMessage(begin, end, max_parsed_pos, nullptr, hilite,
            (description.empty() ? std::string() : std::string(". ")) + "Multi-statements are not allowed");
        return nullptr;
    }

    /// Parse error.
    if (!parse_res)
    {
        out_error_message = getSyntaxErrorMessage(begin, end, max_parsed_pos, expected, hilite, description);
        return nullptr;
    }

    pos = token_iterator->begin;
    return res;
}


ASTPtr parseQueryAndMovePosition(
    IParser & parser,
    const char * & pos,
    const char * end,
    const std::string & description,
    bool allow_multi_statements)
{
    std::string error_message;
    ASTPtr res = tryParseQuery(parser, pos, end, error_message, false, description, allow_multi_statements);

    if (res)
        return res;

    throw Exception(error_message, ErrorCodes::SYNTAX_ERROR);
}


ASTPtr parseQuery(
    IParser & parser,
    const char * begin,
    const char * end,
    const std::string & description)
{
    auto pos = begin;
    return parseQueryAndMovePosition(parser, pos, end, description, false);
}


std::pair<const char *, bool> splitMultipartQuery(const std::string & queries, std::vector<std::string> & queries_list)
{
    ASTPtr ast;

    const char * begin = queries.data(); /// begin of current query
    const char * pos = begin; /// parser moves pos from begin to the end of current query
    const char * end = begin + queries.size();

    ParserQuery parser(end);

    queries_list.clear();

    while (pos < end)
    {
        begin = pos;

        ast = parseQueryAndMovePosition(parser, pos, end, "", true);
        if (!ast)
            break;

        ASTInsertQuery * insert = typeid_cast<ASTInsertQuery *>(ast.get());

        if (insert && insert->data)
        {
            /// Data for INSERT is broken on new line
            pos = insert->data;
            while (*pos && *pos != '\n')
                ++pos;
            insert->end = pos;
        }

        queries_list.emplace_back(queries.substr(begin - queries.data(), pos - begin));

        while (isWhitespaceASCII(*pos) || *pos == ';')
            ++pos;
    }

    return std::make_pair(begin, pos == end);
}

}
