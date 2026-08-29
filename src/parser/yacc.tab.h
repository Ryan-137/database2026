/* A Bison parser, made by GNU Bison 3.5.1.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2020 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* Undocumented macros, especially those whose name start with YY_,
   are private implementation details.  Do not rely on them.  */

#ifndef YY_YY_HOME_LIME_DOCUMENTS_DB2026_QRY_SRC_PARSER_YACC_TAB_H_INCLUDED
# define YY_YY_HOME_LIME_DOCUMENTS_DB2026_QRY_SRC_PARSER_YACC_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif
/* "%code requires" blocks.  */
#line 1 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"

#include <memory>
#include <string>
#include "ast.h"
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

#line 58 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.h"

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    SHOW = 258,
    TABLES = 259,
    CREATE = 260,
    STATIC_CHECKPOINT = 261,
    TABLE = 262,
    DROP = 263,
    DESC = 264,
    INSERT = 265,
    INTO = 266,
    VALUES = 267,
    DELETE = 268,
    FROM = 269,
    ASC = 270,
    ORDER = 271,
    BY = 272,
    AS = 273,
    GROUP = 274,
    HAVING = 275,
    LIMIT = 276,
    WHERE = 277,
    UPDATE = 278,
    SET = 279,
    SELECT = 280,
    INT = 281,
    CHAR = 282,
    FLOAT = 283,
    INDEX = 284,
    AND = 285,
    JOIN = 286,
    EXIT = 287,
    HELP = 288,
    TXN_BEGIN = 289,
    TXN_COMMIT = 290,
    TXN_ABORT = 291,
    TXN_ROLLBACK = 292,
    ORDER_BY = 293,
    ENABLE_NESTLOOP = 294,
    ENABLE_SORTMERGE = 295,
    EXPLAIN = 296,
    ANALYZE = 297,
    ON = 298,
    UNION = 299,
    COUNT = 300,
    SUM = 301,
    MAX = 302,
    MIN = 303,
    AVG = 304,
    TRANSACTION = 305,
    ISOLATION = 306,
    LEVEL = 307,
    SNAPSHOT = 308,
    SERIALIZABLE = 309,
    LEQ = 310,
    NEQ = 311,
    GEQ = 312,
    T_EOF = 313,
    IDENTIFIER = 314,
    VALUE_STRING = 315,
    VALUE_INT = 316,
    VALUE_FLOAT = 317,
    VALUE_BOOL = 318
  };
#endif

/* Value type.  */

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif



int yyparse (yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg);

#endif /* !YY_YY_HOME_LIME_DOCUMENTS_DB2026_QRY_SRC_PARSER_YACC_TAB_H_INCLUDED  */
