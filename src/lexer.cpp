// RajSQL: lexer.cpp

#include "lexer.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace rajsql {

static bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
static bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::vector<Token> lex_sql(const std::string& sql) {
    std::vector<Token> tokens;
    size_t i = 0;
    auto push = [&](TokenType t, std::string text, size_t pos) {
        Token tok;
        tok.type = t;
        tok.text = std::move(text);
        tok.pos = pos;
        tokens.push_back(std::move(tok));
    };

    while (i < sql.size()) {
        char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        // comments: -- to end of line
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') ++i;
            continue;
        }
        size_t start = i;
        if (is_ident_start(c)) {
            ++i;
            while (i < sql.size() && is_ident_char(sql[i])) ++i;
            std::string word = sql.substr(start, i - start);
            push(TokenType::Identifier, std::move(word), start);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
            bool isReal = false;
            ++i;
            while (i < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[i])) ||
                                      sql[i] == '.')) {
                if (sql[i] == '.') isReal = true;
                ++i;
            }
            std::string num = sql.substr(start, i - start);
            if (isReal) {
                Token tok;
                tok.type = TokenType::RealLiteral;
                tok.text = num;
                tok.literal = Value::make_real(std::strtod(num.c_str(), nullptr));
                tok.pos = start;
                tokens.push_back(std::move(tok));
            } else {
                Token tok;
                tok.type = TokenType::IntLiteral;
                tok.text = num;
                tok.literal = Value::make_int(std::strtoll(num.c_str(), nullptr, 10));
                tok.pos = start;
                tokens.push_back(std::move(tok));
            }
            continue;
        }
        if (c == '\'') {
            ++i;
            std::string text;
            while (true) {
                if (i >= sql.size())
                    throw RajSQLError("unterminated string literal at position " +
                                      std::to_string(start));
                if (sql[i] == '\'') {
                    // '' escapes a single quote
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        text += '\'';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                text += sql[i++];
            }
            Token tok;
            tok.type = TokenType::StringLiteral;
            tok.text = std::move(text);
            tok.literal = Value::make_text(tok.text);
            tok.pos = start;
            tokens.push_back(std::move(tok));
            continue;
        }
        switch (c) {
            case ',': push(TokenType::Comma, ",", start); ++i; continue;
            case ';': push(TokenType::Semicolon, ";", start); ++i; continue;
            case '(': push(TokenType::LeftParen, "(", start); ++i; continue;
            case ')': push(TokenType::RightParen, ")", start); ++i; continue;
            case '*': push(TokenType::Star, "*", start); ++i; continue;
            case '.': push(TokenType::Dot, ".", start); ++i; continue;
            case '+': push(TokenType::Plus, "+", start); ++i; continue;
            case '-': push(TokenType::Minus, "-", start); ++i; continue;
            case '/': push(TokenType::Slash, "/", start); ++i; continue;
            case '=': push(TokenType::Eq, "=", start); ++i; continue;
            case '!':
                if (i + 1 < sql.size() && sql[i + 1] == '=') {
                    push(TokenType::Ne, "!=", start);
                    i += 2;
                    continue;
                }
                throw RajSQLError(std::string("unexpected character '!' at position ") +
                                  std::to_string(start));
            case '<':
                if (i + 1 < sql.size() && sql[i + 1] == '=') {
                    push(TokenType::Le, "<=", start);
                    i += 2;
                } else if (i + 1 < sql.size() && sql[i + 1] == '>') {
                    push(TokenType::Ne, "<>", start);
                    i += 2;
                } else {
                    push(TokenType::Lt, "<", start);
                    ++i;
                }
                continue;
            case '>':
                if (i + 1 < sql.size() && sql[i + 1] == '=') {
                    push(TokenType::Ge, ">=", start);
                    i += 2;
                } else {
                    push(TokenType::Gt, ">", start);
                    ++i;
                }
                continue;
            default:
                throw RajSQLError(std::string("unexpected character '") + c +
                                  "' at position " + std::to_string(start));
        }
    }
    Token end;
    end.type = TokenType::End;
    end.pos = i;
    tokens.push_back(std::move(end));
    return tokens;
}

static bool iequals(const std::string& a, const char* b) {
    size_t len = std::strlen(b);
    if (a.size() != len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool is_keyword(const Token& token, const char* keyword) {
    return token.type == TokenType::Identifier && iequals(token.text, keyword);
}

}  // namespace rajsql
