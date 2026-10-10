// RajSQL: lexer.hpp
// Tokenizer: turns raw SQL text into a flat token stream for the parser.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "value.hpp"

namespace rajsql {

enum class TokenType {
    Identifier,   // bare identifier or keyword (case-insensitive)
    IntLiteral,
    RealLiteral,
    StringLiteral,  // 'single quoted'
    Comma,
    Semicolon,
    LeftParen,
    RightParen,
    Star,
    Dot,
    Plus,
    Minus,
    Slash,
    Eq,    // =
    Ne,    // != or <>
    Lt,    // <
    Le,    // <=
    Gt,    // >
    Ge,    // >=
    End,
};

struct Token {
    TokenType type;
    std::string text;   // raw text (identifier case preserved)
    Value literal;      // for IntLiteral / RealLiteral / StringLiteral
    size_t pos = 0;     // offset into the source (for error messages)
};

// Throws RajSQLError on unterminated strings or illegal characters.
std::vector<Token> lex_sql(const std::string& sql);

// Case-insensitive keyword compare.
bool is_keyword(const Token& token, const char* keyword);

}  // namespace rajsql
