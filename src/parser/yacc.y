%code requires {
#include <memory>
#include <string>
#include "ast.h"
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
}

%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>
#include <string>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc, yyscan_t scanner);

void yyerror(YYLTYPE *locp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result,
             std::string *error_msg, const char* s) {
    (void)scanner;
    (void)parse_result;
    if (error_msg != nullptr) {
        *error_msg = "Parser Error at line " + std::to_string(locp->first_line) +
                     " column " + std::to_string(locp->first_column) + ": " + s;
    }
}

using namespace ast;
%}

// request a pure (reentrant) parser
%define api.pure full
%param { yyscan_t scanner }
%parse-param { std::shared_ptr<ast::TreeNode> *parse_result }
%parse-param { std::string *error_msg }
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE STATIC_CHECKPOINT TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY AS GROUP HAVING LIMIT
WHERE UPDATE SET SELECT INT CHAR FLOAT INDEX AND JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE EXPLAIN ANALYZE ON UNION COUNT SUM MAX MIN AVG
%token TRANSACTION ISOLATION LEVEL SNAPSHOT SERIALIZABLE
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt explainStmt selectStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName opt_alias
%type <sv_strs> colNameList
%type <sv_col> col aggregate selectItem condition_lhs
%type <sv_cols> colList selector selectItemList opt_group_clause
%type <sv_table_ref> tableRef
%type <sv_from_clause> fromClause
%type <sv_union> unionQuery
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_update_expr> updateExpr
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause opt_having_clause
%type <sv_orderby> order_item
%type <sv_orderbys> order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_agg_func> agg_func
%type <sv_int> opt_limit_clause
%type <sv_setKnobType> set_knob_type
%type <sv_isolation_level> isolation_level

%%
start:
        stmt ';'
    {
        *parse_result = $1;
        YYACCEPT;
    }
    |   stmt T_EOF
    {
        *parse_result = $1;
        YYACCEPT;
    }
    |   HELP
    {
        *parse_result = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    |   explainStmt
    ;

explainStmt:
        EXPLAIN ANALYZE selectStmt
    {
        // 任务四入口：EXPLAIN ANALYZE 只解释 SELECT，不直接输出查询结果
        $$ = std::make_shared<ExplainAnalyzeStmt>(std::dynamic_pointer_cast<SelectStmt>($3));
    }
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL isolation_level
    {
        $$ = std::make_shared<SetTransactionIsolationStmt>($5);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   CREATE STATIC_CHECKPOINT
    {
        $$ = std::make_shared<CreateStaticCheckpoint>();
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   selectStmt
    ;

selectStmt:
        SELECT selector FROM fromClause optWhereClause opt_group_clause opt_having_clause opt_order_clause opt_limit_clause
    {
        // FROM 子句保留真实表名、别名和 JOIN ON 条件，后续 Analyze/Planner 再做下推优化
        $$ = std::make_shared<SelectStmt>($2, $4, $5, $6, $7, $8, $9);
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<ast::Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   '-' VALUE_INT
    {
        $$ = std::make_shared<IntLit>(-$2);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   '-' VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>(-$2);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    ;

condition:
        condition_lhs op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { $$ = {}; }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   aggregate
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<ast::SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' updateExpr
    {
        $$ = std::make_shared<ast::SetClause>($1, $3);
    }
    ;

updateExpr:
        value
    {
        $$ = std::make_shared<ast::UpdateExpr>($1);
    }
    |   col
    {
        $$ = std::make_shared<ast::UpdateExpr>($1);
    }
    |   col '+' value
    {
        $$ = std::make_shared<ast::UpdateExpr>(ast::UpdateExprKind::ADD,
             std::make_shared<ast::UpdateExpr>($1), std::make_shared<ast::UpdateExpr>($3));
    }
    |   col '-' value
    {
        $$ = std::make_shared<ast::UpdateExpr>(ast::UpdateExprKind::SUB,
             std::make_shared<ast::UpdateExpr>($1), std::make_shared<ast::UpdateExpr>($3));
    }
    ;

selector:
        '*' { $$ = {}; }
    |   selectItemList
    ;

selectItemList:
        selectItem { $$ = std::vector<std::shared_ptr<Col>>{$1}; }
    |   selectItemList ',' selectItem { $$.push_back($3); }
    ;

selectItem:
        col opt_alias { $1->alias = $2; $$ = $1; }
    |   aggregate opt_alias { $1->alias = $2; $$ = $1; }
    ;

opt_alias:
        /* epsilon */ { $$ = ""; }
    |   AS colName { $$ = $2; }
    ;

aggregate:
        COUNT '(' '*' ')' { $$ = std::make_shared<Col>(AGG_COUNT, "", "*", true); }
    |   COUNT '(' col ')' { $$ = std::make_shared<Col>(AGG_COUNT, $3->tab_name, $3->col_name, false); }
    |   agg_func '(' col ')' { $$ = std::make_shared<Col>($1, $3->tab_name, $3->col_name, false); }
    ;

agg_func:
        MAX { $$ = AGG_MAX; }
    |   MIN { $$ = AGG_MIN; }
    |   SUM { $$ = AGG_SUM; }
    |   AVG { $$ = AGG_AVG; }
    ;

condition_lhs:
        col { $$ = $1; }
    |   aggregate { $$ = $1; }
    ;

tableRef:
        tbName
    {
        // 无别名时，输出和解析都继续使用真实表名
        $$ = std::make_shared<TableRef>($1, "");
    }
    |   tbName IDENTIFIER
    {
        // 支持 FROM customers c 这种任务四常用表别名写法
        $$ = std::make_shared<TableRef>($1, $2);
    }
    |   tbName AS IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($1, $3);
    }
    |   '(' unionQuery ')' AS IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($2, $5);
    }
    ;

unionQuery:
        selectStmt UNION selectStmt
    {
        $$ = std::make_shared<UnionStmt>(std::dynamic_pointer_cast<SelectStmt>($1),
                                         std::dynamic_pointer_cast<SelectStmt>($3));
    }
    |   unionQuery UNION selectStmt
    {
        $1->branches.push_back(std::dynamic_pointer_cast<SelectStmt>($3));
        $$ = $1;
    }
    ;

fromClause:
        tableRef
    {
        $$ = std::make_shared<FromClause>($1);
    }
    |   fromClause ',' tableRef
    {
        // 兼容原有逗号分隔多表查询
        $1->tables.push_back($3);
        $$ = $1;
    }
    |   fromClause JOIN tableRef
    {
        // 兼容原有无 ON 的 JOIN，语义等价于没有连接条件的多表连接
        $1->tables.push_back($3);
        $$ = $1;
    }
    |   fromClause JOIN tableRef ON whereClause
    {
        // JOIN ON 条件单独保存，后续计划生成时应进入 Join 节点
        $1->tables.push_back($3);
        $1->join_conds.insert($1->join_conds.end(), $5.begin(), $5.end());
        $$ = $1;
    }
    ;

opt_group_clause:
        /* epsilon */ { $$ = {}; }
    |   GROUP BY colList { $$ = $3; }
    ;

opt_having_clause:
        /* epsilon */ { $$ = {}; }
    |   HAVING whereClause { $$ = $2; }
    ;

opt_order_clause:
        ORDER BY order_clause { $$ = $3; }
    |   /* epsilon */ { $$ = {}; }
    ;

order_clause:
        order_item { $$ = std::vector<std::shared_ptr<OrderBy>>{$1}; }
    |   order_clause ',' order_item { $$.push_back($3); }
    ;

order_item:
        condition_lhs opt_asc_desc { $$ = std::make_shared<OrderBy>($1, $2); }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;

opt_limit_clause:
        /* epsilon */ { $$ = -1; }
    |   LIMIT VALUE_INT { $$ = $2; }
    ;

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

isolation_level:
    SNAPSHOT ISOLATION { $$ = TxnIsolationLevel::SNAPSHOT_ISOLATION; }
    |   SERIALIZABLE { $$ = TxnIsolationLevel::SERIALIZABLE; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
