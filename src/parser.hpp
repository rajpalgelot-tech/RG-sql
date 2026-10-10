// RajSQL: parser.hpp
// Recursive-descent parser producing a small statement AST.
//
// Supported grammar (v1):
//   statement := create | drop | insert | select | update | delete | txn
//   create    := CREATE TABLE ident '(' column (',' column)* ')'
//   column    := ident type
//   type      := INTEGER | INT | REAL | FLOAT | DOUBLE | TEXT | VARCHAR '(' int ')'
//   select    := SELECT items FROM table join* [WHERE expr]
//                [ORDER BY ident [ASC|DESC] (',' ident [ASC|DESC])*] [LIMIT int]
//   join      := JOIN table ON expr          (inner, left-deep)
//   items     := '*' | item (',' item)*
//   item      := expr [AS ident] | ident '.' '*' (no AS on star)
//   expr      := or_expr        with precedence: OR < AND < comparison < add/sub
//   func      := COUNT '(' '*' ')' | (SUM|AVG|MIN|MAX|COUNT) '(' ident ')'

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lexer.hpp"

namespace rajsql {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind { Literal, Column, Func, UnaryNeg, UnaryNot, Binary };
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge, And, Or, Add, Sub, Mul, Div };
    enum class FuncKind { CountStar, Count, Sum, Avg, Min, Max };

    Kind kind;
    Value literal;              // Kind::Literal
    std::string table;          // Kind::Column  (may be empty = unqualified)
    std::string column;         // Kind::Column / Func argument
    FuncKind agg{FuncKind::CountStar};  // Kind::Func
    Op op{Op::Eq};             // Kind::Binary
    ExprPtr left;               // Kind::Binary / UnaryNeg / UnaryNot operand
    ExprPtr right;              // Kind::Binary

    static ExprPtr literal_value(Value v);
    static ExprPtr column_ref(std::string table, std::string column);
    static ExprPtr func(FuncKind kind, std::string column);
    static ExprPtr negate(ExprPtr operand);
    static ExprPtr not_expr(ExprPtr operand);
    static ExprPtr binary(Op op, ExprPtr lhs, ExprPtr rhs);
};

struct SelectItem {
    ExprPtr expr;        // nullptr only when star (see below)
    bool star = false;   // bare *
    std::string table_star;  // t.* (star scoped to one table)
    std::string alias;   // output name (also set for plain columns)
};

struct JoinClause {
    std::string table;
    ExprPtr on;          // join condition
};

struct OrderByItem {
    std::string table;   // optional qualifier ("students" in students.name)
    std::string column;
    bool descending = false;
};

struct Statement {
    enum class Type {
        CreateTable,
        DropTable,
        Insert,
        Select,
        Update,
        Delete,
        Begin,
        Commit,
        Rollback,
    };

    Type type;

    // CreateTable / DropTable
    std::string table_name;
    struct ColumnDef {
        std::string name;
        ColumnType sql_type;
    };
    std::vector<ColumnDef> columns;

    // Insert
    std::vector<std::string> insert_columns;   // empty = all columns in order
    std::vector<std::vector<ExprPtr>> insert_rows;

    // Select
    std::vector<SelectItem> items;
    std::string from_table;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<OrderByItem> order_by;
    int limit{-1};

    // Update
    std::vector<std::pair<std::string, ExprPtr>> set_clauses;

    Statement(Statement::Type t) : type(t) {}
};

// Parses ALL statements in the input (split on ';').
// Throws RajSQLError with position info on syntax errors.
std::vector<std::unique_ptr<Statement>> parse_sql(const std::string& sql);

}  // namespace rajsql
