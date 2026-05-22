#ifndef TDB_SQL_LEXER_H
#define TDB_SQL_LEXER_H

#include "tdb/sql/token.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace tdb::sql {

class Lexer {
public:
    explicit Lexer(const std::string &input);

    Token next_token();
    std::vector<Token> tokenize_all();

    uint32_t line() const { return line_; }
    uint32_t col() const { return col_; }

private:
    std::string input_;
    size_t      pos_;
    uint32_t    line_;
    uint32_t    col_;

    char peek() const;
    char peek_ahead(size_t offset) const;
    char advance();
    void skip_whitespace();
    void skip_line_comment();
    void skip_block_comment();

    Token read_number();
    Token read_string();
    Token read_identifier_or_keyword();
    Token read_quoted_identifier();
    Token read_blob_literal();

    static const std::unordered_map<std::string, TokenType> keywords_;
};

} // namespace tdb::sql

#endif // TDB_SQL_LEXER_H
