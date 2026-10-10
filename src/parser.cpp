// RajSQL: parser.cpp

#include "parser.hpp"

#include <utility>

namespace rajsql {

ExprPtr Expr::literal_value(Value v) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Literal;
    e->literal = std::move(v);
    return e;
}

ExprPtr Expr::column_ref(std::string table, std::string column) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Column;
    e->table = std::move(table);
    e->column = std::move(column);
    return e;
}

ExprPtr Expr::func(FuncKind kind, std::string column) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Func;
    e->agg = kind;
    e->column = std::move(column);
    return e;
}

ExprPtr Expr::negate(ExprPtr operand) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::UnaryNeg;
    e->left = std::move(operand);
    return e;
}

ExprPtr Expr::not_expr(ExprPtr operand) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::UnaryNot;
    e->left = std::move(operand);
    return e;
}

ExprPtr Expr::binary(Op op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Binary;
    e->op = op;
    e->left = std::move(lhs);
    e->right = std::move(rhs);
    return e;
}

namespace {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::vector<std::unique_ptr<Statement>> parse_program() {
        std::vector<std::unique_ptr<Statement>> out;
        while (!at_end()) {
            expect_semicolons();
            if (at_end()) break;
            out.push_back(parse_statement());
            if (peek().type != TokenType::Semicolon && !at_end())
                throw error_here("expected ';' after statement");
        }
        return out;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek(size_t ahead = 0) const {
        size_t p = pos_ + ahead;
        if (p >= tokens_.size()) p = tokens_.size() - 1;
        return tokens_[p];
    }
    bool at_end() const { return peek().type == TokenType::End; }
    const Token& advance() { return tokens_[pos_++]; }

    RajSQLError error_here(const std::string& msg) {
        return RajSQLError("syntax error at position " + std::to_string(peek().pos) +
                          " near '" + peek().text + "': " + msg);
    }

    void expect_semicolons() {
        while (peek().type == TokenType::Semicolon) advance();
    }

    bool match_keyword(const char* kw) {
        if (is_keyword(peek(), kw)) {
            advance();
            return true;
        }
        return false;
    }

    void expect_keyword(const char* kw) {
        if (!match_keyword(kw)) throw error_here(std::string("expected ") + kw);
    }

    std::string expect_ident(const char* what) {
        if (peek().type != TokenType::Identifier)
            throw error_here(std::string("expected ") + what + " identifier");
        return advance().text;
    }

    std::unique_ptr<Statement> parse_statement() {
        if (is_keyword(peek(), "CREATE")) return parse_create();
        if (is_keyword(peek(), "DROP")) return parse_drop();
        if (is_keyword(peek(), "INSERT")) return parse_insert();
        if (is_keyword(peek(), "SELECT")) return parse_select();
        if (is_keyword(peek(), "UPDATE")) return parse_update();
        if (is_keyword(peek(), "DELETE")) return parse_delete();
        if (is_keyword(peek(), "BEGIN")) {
            advance();
            auto stmt = std::make_unique<Statement>(Statement::Type::Begin);
            return stmt;
        }
        if (is_keyword(peek(), "COMMIT")) {
            advance();
            return std::make_unique<Statement>(Statement::Type::Commit);
        }
        if (is_keyword(peek(), "ROLLBACK")) {
            advance();
            return std::make_unique<Statement>(Statement::Type::Rollback);
        }
        throw error_here("unknown statement (expected SELECT, INSERT, UPDATE, DELETE, "
                         "CREATE, DROP, BEGIN, COMMIT or ROLLBACK)");
    }

    std::unique_ptr<Statement> parse_create() {
        expect_keyword("CREATE");
        expect_keyword("TABLE");
        auto stmt = std::make_unique<Statement>(Statement::Type::CreateTable);
        stmt->table_name = expect_ident("table name");
        if (peek().type != TokenType::LeftParen) throw error_here("expected '(' after table name");
        advance();
        do {
            std::string name = expect_ident("column name");
            stmt->columns.push_back({name, parse_type()});
        } while (match_comma());
        if (peek().type != TokenType::RightParen) throw error_here("expected ')' after column list");
        advance();
        return stmt;
    }

    ColumnType parse_type() {
        const Token& t = peek();
        if (t.type != TokenType::Identifier) throw error_here("expected column type");
        std::string type = t.text;
        for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        advance();
        if (type == "integer" || type == "int") return ColumnType::Integer;
        if (type == "real" || type == "float" || type == "double") return ColumnType::Real;
        if (type == "text" || type == "varchar") {
            if (type == "varchar") {  // swallow the (n)
                if (peek().type == TokenType::LeftParen) {
                    advance();
                    if (peek().type != TokenType::IntLiteral)
                        throw error_here("expected length in VARCHAR(n)");
                    advance();
                    if (peek().type != TokenType::RightParen)
                        throw error_here("expected ')' after VARCHAR length");
                    advance();
                }
            }
            return ColumnType::Text;
        }
        throw error_here("unknown column type '" + t.text +
                         "' (supported: INTEGER, REAL, TEXT)");
    }

    bool match_comma() {
        if (peek().type == TokenType::Comma) {
            advance();
            return true;
        }
        return false;
    }

    std::unique_ptr<Statement> parse_drop() {
        expect_keyword("DROP");
        expect_keyword("TABLE");
        auto stmt = std::make_unique<Statement>(Statement::Type::DropTable);
        stmt->table_name = expect_ident("table name");
        return stmt;
    }

    std::unique_ptr<Statement> parse_insert() {
        expect_keyword("INSERT");
        expect_keyword("INTO");
        auto stmt = std::make_unique<Statement>(Statement::Type::Insert);
        stmt->table_name = expect_ident("table name");
        if (peek().type == TokenType::LeftParen) {
            advance();
            do {
                stmt->insert_columns.push_back(expect_ident("column name"));
            } while (match_comma());
            if (peek().type != TokenType::RightParen)
                throw error_here("expected ')' after column list");
            advance();
        }
        expect_keyword("VALUES");
        do {
            if (peek().type != TokenType::LeftParen)
                throw error_here("expected '(' before VALUES row");
            advance();
            std::vector<ExprPtr> row;
            do {
                row.push_back(parse_expr());
            } while (match_comma());
            if (peek().type != TokenType::RightParen)
                throw error_here("expected ')' after VALUES row");
            advance();
            stmt->insert_rows.push_back(std::move(row));
        } while (match_comma());
        return stmt;
    }

    std::unique_ptr<Statement> parse_select() {
        expect_keyword("SELECT");
        auto stmt = std::make_unique<Statement>(Statement::Type::Select);
        do {
            SelectItem item;
            if (peek().type == TokenType::Star) {
                advance();
                item.star = true;
            } else {
                item.expr = parse_expr();
                if (is_keyword(peek(), "AS")) {
                    advance();
                    item.alias = expect_ident("alias");
                }
            }
            stmt->items.push_back(std::move(item));
        } while (match_comma());

        expect_keyword("FROM");
        stmt->from_table = expect_ident("table name");
        while (is_keyword(peek(), "JOIN")) {
            advance();
            JoinClause join;
            join.table = expect_ident("table name");
            expect_keyword("ON");
            join.on = parse_expr();
            stmt->joins.push_back(std::move(join));
        }

        if (match_keyword("WHERE")) stmt->where = parse_expr();

        if (match_keyword("ORDER")) {
            expect_keyword("BY");
            do {
                OrderByItem ob;
                ob.column = expect_ident("order-by column");
                if (peek().type == TokenType::Dot) {
                    // qualified column: table.column
                    ob.table = ob.column;
                    advance();
                    ob.column = expect_ident("column name after '.'");
                }
                if (match_keyword("DESC")) ob.descending = true;
                else match_keyword("ASC");
                stmt->order_by.push_back(std::move(ob));
            } while (match_comma());
        }

        if (match_keyword("LIMIT")) {
            if (peek().type != TokenType::IntLiteral)
                throw error_here("expected integer after LIMIT");
            stmt->limit = static_cast<int>(advance().literal.int_v);
            if (stmt->limit < 0) throw error_here("LIMIT must be non-negative");
        }
        return stmt;
    }

    std::unique_ptr<Statement> parse_update() {
        expect_keyword("UPDATE");
        auto stmt = std::make_unique<Statement>(Statement::Type::Update);
        stmt->table_name = expect_ident("table name");
        expect_keyword("SET");
        do {
            std::string col = expect_ident("column name");
            if (peek().type != TokenType::Eq)
                throw error_here("expected '=' in SET clause");
            advance();
            stmt->set_clauses.emplace_back(col, parse_expr());
        } while (match_comma());
        if (match_keyword("WHERE")) stmt->where = parse_expr();
        return stmt;
    }

    std::unique_ptr<Statement> parse_delete() {
        expect_keyword("DELETE");
        expect_keyword("FROM");
        auto stmt = std::make_unique<Statement>(Statement::Type::Delete);
        stmt->table_name = expect_ident("table name");
        if (match_keyword("WHERE")) stmt->where = parse_expr();
        return stmt;
    }

    // ── Expressions ────────────────────────────────────────────────────
    // precedence chain: expr := or → and → comparison → addsub → unary → primary

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (is_keyword(peek(), "OR")) {
            advance();
            left = Expr::binary(Expr::Op::Or, std::move(left), parse_and());
        }
        return left;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_comparison();
        while (is_keyword(peek(), "AND")) {
            advance();
            left = Expr::binary(Expr::Op::And, std::move(left), parse_comparison());
        }
        return left;
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_addsub();
        const Token& t = peek();
        if (t.type == TokenType::Eq) {
            advance();
            return Expr::binary(Expr::Op::Eq, std::move(left), parse_addsub());
        }
        if (t.type == TokenType::Ne) {
            advance();
            return Expr::binary(Expr::Op::Ne, std::move(left), parse_addsub());
        }
        if (t.type == TokenType::Lt) {
            advance();
            return Expr::binary(Expr::Op::Lt, std::move(left), parse_addsub());
        }
        if (t.type == TokenType::Le) {
            advance();
            return Expr::binary(Expr::Op::Le, std::move(left), parse_addsub());
        }
        if (t.type == TokenType::Gt) {
            advance();
            return Expr::binary(Expr::Op::Gt, std::move(left), parse_addsub());
        }
        if (t.type == TokenType::Ge) {
            advance();
            return Expr::binary(Expr::Op::Ge, std::move(left), parse_addsub());
        }
        return left;
    }

    ExprPtr parse_addsub() {
        ExprPtr left = parse_muldiv();
        while (true) {
            if (peek().type == TokenType::Plus) {
                advance();
                left = Expr::binary(Expr::Op::Add, std::move(left), parse_muldiv());
            } else if (peek().type == TokenType::Minus) {
                advance();
                left = Expr::binary(Expr::Op::Sub, std::move(left), parse_muldiv());
            } else {
                break;
            }
        }
        return left;
    }

    ExprPtr parse_muldiv() {
        ExprPtr left = parse_unary();
        while (true) {
            if (peek().type == TokenType::Star) {
                advance();
                left = Expr::binary(Expr::Op::Mul, std::move(left), parse_unary());
            } else if (peek().type == TokenType::Slash) {
                advance();
                left = Expr::binary(Expr::Op::Div, std::move(left), parse_unary());
            } else {
                break;
            }
        }
        return left;
    }

    ExprPtr parse_unary() {
        if (peek().type == TokenType::Minus) {
            advance();
            return Expr::negate(parse_unary());
        }
        if (is_keyword(peek(), "NOT")) {
            advance();
            return Expr::not_expr(parse_unary());
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.type) {
            case TokenType::IntLiteral:
            case TokenType::RealLiteral:
            case TokenType::StringLiteral: {
                Value lit = advance().literal;
                return Expr::literal_value(std::move(lit));
            }
            case TokenType::LeftParen: {
                advance();
                ExprPtr inner = parse_expr();
                if (peek().type != TokenType::RightParen)
                    throw error_here("expected ')'");
                advance();
                return inner;
            }
            case TokenType::Identifier: {
                if (is_keyword(t, "NULL")) {
                    advance();
                    return Expr::literal_value(Value::null());
                }
                // function call?
                if (peek(1).type == TokenType::LeftParen) {
                    return parse_func();
                }
                std::string first = advance().text;
                if (peek().type == TokenType::Dot) {
                    advance();
                    if (peek().type == TokenType::Star) {
                        throw error_here("t.* in expressions is not supported");
                    }
                    std::string col = expect_ident("column name after '.'");
                    return Expr::column_ref(first, col);
                }
                return Expr::column_ref("", first);
            }
            default:
                throw error_here("expected expression");
        }
    }

    ExprPtr parse_func() {
        std::string name = advance().text;
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        Expr::FuncKind kind;
        bool takes_column = true;
        if (lower == "count" && peek(1).type == TokenType::Star) kind = Expr::FuncKind::CountStar, takes_column = false;
        else if (lower == "count") kind = Expr::FuncKind::Count;
        else if (lower == "sum") kind = Expr::FuncKind::Sum;
        else if (lower == "avg") kind = Expr::FuncKind::Avg;
        else if (lower == "min") kind = Expr::FuncKind::Min;
        else if (lower == "max") kind = Expr::FuncKind::Max;
        else throw error_here("unknown function '" + name + "'");
        if (peek().type != TokenType::LeftParen) throw error_here("expected '(' after " + name);
        advance();
        std::string column;
        if (takes_column) {
            column = expect_ident("aggregate argument column");
        } else {
            if (peek().type != TokenType::Star) throw error_here("expected '*' in COUNT(*)");
            advance();
        }
        if (peek().type != TokenType::RightParen) throw error_here("expected ')' after function");
        advance();
        return Expr::func(kind, column);
    }
};

}  // namespace

std::vector<std::unique_ptr<Statement>> parse_sql(const std::string& sql) {
    return Parser(lex_sql(sql)).parse_program();
}

}  // namespace rajsql
