/* A Bison parser, made by GNU Bison 3.5.1.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Undocumented macros, especially those whose name start with YY_,
   are private implementation details.  Do not rely on them.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.5.1"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 11 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"

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

#line 92 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 1
#endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
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

#line 145 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"

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



#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))

/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif

#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                            \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  59
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   208

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  75
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  47
/* YYNRULES -- Number of rules.  */
#define YYNRULES  120
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  216

#define YYUNDEFTOK  2
#define YYMAXUTOK   318


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      66,    67,    74,    73,    68,    69,    70,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    64,
      71,    65,    72,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,    87,    87,    92,    97,   102,   107,   115,   116,   117,
     118,   119,   120,   124,   132,   136,   140,   144,   151,   155,
     162,   166,   173,   177,   181,   185,   189,   193,   200,   204,
     208,   212,   216,   224,   228,   235,   239,   246,   253,   257,
     261,   268,   272,   279,   283,   287,   291,   295,   299,   306,
     313,   314,   321,   325,   332,   336,   343,   347,   354,   358,
     362,   366,   370,   374,   381,   385,   389,   396,   400,   407,
     414,   418,   422,   427,   435,   436,   440,   441,   445,   446,
     450,   451,   455,   456,   457,   461,   462,   463,   464,   468,
     469,   473,   478,   483,   487,   494,   499,   507,   511,   517,
     523,   533,   534,   538,   539,   543,   544,   548,   549,   553,
     557,   558,   559,   563,   564,   568,   569,   573,   574,   577,
     579
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 1
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "SHOW", "TABLES", "CREATE",
  "STATIC_CHECKPOINT", "TABLE", "DROP", "DESC", "INSERT", "INTO", "VALUES",
  "DELETE", "FROM", "ASC", "ORDER", "BY", "AS", "GROUP", "HAVING", "LIMIT",
  "WHERE", "UPDATE", "SET", "SELECT", "INT", "CHAR", "FLOAT", "INDEX",
  "AND", "JOIN", "EXIT", "HELP", "TXN_BEGIN", "TXN_COMMIT", "TXN_ABORT",
  "TXN_ROLLBACK", "ORDER_BY", "ENABLE_NESTLOOP", "ENABLE_SORTMERGE",
  "EXPLAIN", "ANALYZE", "ON", "UNION", "COUNT", "SUM", "MAX", "MIN", "AVG",
  "TRANSACTION", "ISOLATION", "LEVEL", "SNAPSHOT", "SERIALIZABLE", "LEQ",
  "NEQ", "GEQ", "T_EOF", "IDENTIFIER", "VALUE_STRING", "VALUE_INT",
  "VALUE_FLOAT", "VALUE_BOOL", "';'", "'='", "'('", "')'", "','", "'-'",
  "'.'", "'<'", "'>'", "'+'", "'*'", "$accept", "start", "stmt",
  "explainStmt", "txnStmt", "dbStmt", "setStmt", "ddl", "dml",
  "selectStmt", "fieldList", "colNameList", "field", "type", "valueList",
  "value", "condition", "optWhereClause", "whereClause", "col", "colList",
  "op", "expr", "setClauses", "setClause", "updateExpr", "selector",
  "selectItemList", "selectItem", "opt_alias", "aggregate", "agg_func",
  "condition_lhs", "tableRef", "unionQuery", "fromClause",
  "opt_group_clause", "opt_having_clause", "opt_order_clause",
  "order_clause", "order_item", "opt_asc_desc", "opt_limit_clause",
  "set_knob_type", "isolation_level", "tbName", "colName", YY_NULLPTR
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_int16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,    59,    61,    40,    41,    44,    45,
      46,    60,    62,    43,    42
};
# endif

#define YYPACT_NINF (-163)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-120)

#define yytable_value_is_error(Yyn) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      78,     8,    11,     4,   -45,    16,    29,   -45,    35,   -23,
    -163,  -163,  -163,  -163,  -163,  -163,    15,  -163,    59,     0,
    -163,  -163,  -163,  -163,  -163,  -163,  -163,  -163,    52,  -163,
     -45,   -45,   -45,   -45,  -163,  -163,   -45,   -45,    53,  -163,
    -163,    33,    55,    68,  -163,  -163,  -163,  -163,    69,  -163,
     117,   131,    80,  -163,   117,    83,    94,  -163,   135,  -163,
    -163,  -163,   -45,   100,   101,  -163,   102,   157,   148,   112,
     120,   110,   -27,   112,  -163,    -5,    49,  -163,   115,   112,
    -163,  -163,   112,   112,   112,   109,    49,  -163,  -163,   -13,
    -163,   111,    26,  -163,   113,   114,  -163,   135,  -163,   -18,
     -11,  -163,   116,  -163,    22,  -163,    90,    25,  -163,    32,
      96,  -163,   147,  -163,  -163,    66,   112,  -163,    92,   128,
    -163,  -163,  -163,  -163,   138,   -29,    -5,    -5,   165,   126,
    -163,  -163,  -163,   112,  -163,   121,  -163,  -163,  -163,   112,
    -163,  -163,  -163,  -163,  -163,    43,    65,  -163,    49,  -163,
    -163,  -163,  -163,  -163,  -163,    81,  -163,  -163,     9,  -163,
    -163,   135,   135,   168,   145,  -163,   172,   170,  -163,  -163,
     130,  -163,  -163,  -163,  -163,    96,  -163,  -163,  -163,  -163,
    -163,    96,    96,  -163,  -163,   133,    49,   115,    49,   178,
     129,  -163,  -163,  -163,  -163,   147,  -163,   127,   147,   180,
     177,  -163,   115,    49,   139,  -163,  -163,    47,   134,  -163,
    -163,  -163,  -163,  -163,    49,  -163
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       5,     4,    14,    15,    16,    17,     0,     6,     0,     0,
      12,    10,     7,    11,     8,     9,    31,    18,     0,    23,
       0,     0,     0,     0,   119,    25,     0,     0,     0,   115,
     116,     0,     0,     0,    87,    85,    86,    88,   120,    74,
      80,     0,    75,    76,    80,     0,     0,    55,     0,     1,
       3,     2,     0,     0,     0,    24,     0,     0,    50,     0,
       0,     0,     0,     0,    78,     0,     0,    79,     0,     0,
      13,    19,     0,     0,     0,     0,     0,    29,   120,    50,
      67,     0,     0,    20,     0,     0,    81,     0,    97,    50,
      91,    77,     0,    54,     0,    33,     0,     0,    35,     0,
       0,    52,    51,    89,    90,     0,     0,    30,     0,     0,
     118,    21,    82,    83,     0,     0,     0,     0,   101,     0,
      92,    84,    22,     0,    38,     0,    40,    37,    26,     0,
      27,    47,    43,    45,    48,     0,     0,    41,     0,    62,
      61,    63,    58,    59,    60,     0,    68,    70,    71,    69,
     117,     0,     0,     0,    99,    98,     0,   103,    93,    34,
       0,    36,    44,    46,    28,     0,    53,    64,    65,    49,
      66,     0,     0,    95,    96,     0,     0,     0,     0,   106,
       0,    42,    73,    72,    94,   100,    56,   102,   104,     0,
     113,    39,     0,     0,     0,    32,    57,   112,   105,   107,
     114,   111,   110,   109,     0,   108
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -163,  -163,  -163,  -163,  -163,  -163,  -163,  -163,  -163,   -55,
    -163,   119,    71,  -163,  -163,  -110,    51,   -50,  -142,    -9,
    -163,  -163,  -163,  -163,    85,  -163,  -163,  -163,   132,   151,
      -8,  -163,  -162,    36,  -163,  -163,  -163,  -163,  -163,  -163,
      -7,  -163,  -163,  -163,  -163,    -2,   -63
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,    18,    19,    20,    21,    22,    23,    24,    25,    26,
     104,   107,   105,   137,   146,   147,   111,    87,   112,   113,
     197,   155,   179,    89,    90,   159,    51,    52,    53,    74,
     114,    55,   115,    98,   125,    99,   167,   189,   200,   208,
     209,   213,   205,    42,   121,    56,    57
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      50,    54,    35,    80,    86,    38,    91,   129,   157,    86,
      96,    32,    27,   126,    34,   162,   103,    29,    30,   106,
     108,   108,    43,    44,    45,    46,    47,    36,    63,    64,
      65,    66,    48,    33,    67,    68,    48,    28,   163,   117,
      31,   207,   124,    37,   195,   177,   198,    94,   130,   128,
     127,    49,   207,    91,    34,   116,   211,    58,    60,    59,
      81,    97,   212,    95,    61,   191,    62,    50,    54,   102,
     106,   192,   193,   100,    39,    40,   171,    69,   181,   119,
     120,     1,   182,     2,    70,    41,     3,     4,     5,   132,
     133,     6,   138,   139,    43,    44,    45,    46,    47,   140,
     139,     7,     8,     9,   172,   173,   183,   184,    48,   158,
      10,    11,    12,    13,    14,    15,   134,   135,   136,    16,
      71,   149,   150,   151,   100,   100,    43,    44,    45,    46,
      47,   152,   174,   175,    72,    73,    17,   153,   154,  -119,
      48,   141,   142,   143,   144,    75,   178,   180,    76,    78,
     145,    48,   141,   142,   143,   144,   141,   142,   143,   144,
       9,   145,   164,   165,    79,   145,    82,    83,    84,    85,
      86,    88,    92,    93,    48,   110,   118,   148,   196,   160,
     122,   123,   161,   131,   166,   168,   185,   170,   186,   187,
     188,   190,   194,   206,   199,   202,   201,   203,   204,   176,
     210,   156,   214,   109,   169,    77,     0,   215,   101
};

static const yytype_int16 yycheck[] =
{
       9,     9,     4,    58,    22,     7,    69,    18,   118,    22,
      73,     7,     4,    31,    59,    44,    79,     6,     7,    82,
      83,    84,    45,    46,    47,    48,    49,    11,    30,    31,
      32,    33,    59,    29,    36,    37,    59,    29,    67,    89,
      29,   203,    97,    14,   186,   155,   188,    74,    59,    99,
      68,    74,   214,   116,    59,    68,     9,    42,    58,     0,
      62,    66,    15,    72,    64,   175,    14,    76,    76,    78,
     133,   181,   182,    75,    39,    40,   139,    24,    69,    53,
      54,     3,    73,     5,    51,    50,     8,     9,    10,    67,
      68,    13,    67,    68,    45,    46,    47,    48,    49,    67,
      68,    23,    24,    25,    61,    62,   161,   162,    59,   118,
      32,    33,    34,    35,    36,    37,    26,    27,    28,    41,
      65,    55,    56,    57,   126,   127,    45,    46,    47,    48,
      49,    65,    67,    68,    66,    18,    58,    71,    72,    70,
      59,    60,    61,    62,    63,    14,   155,   155,    68,    66,
      69,    59,    60,    61,    62,    63,    60,    61,    62,    63,
      25,    69,   126,   127,    70,    69,    66,    66,    66,    12,
      22,    59,    52,    63,    59,    66,    65,    30,   187,    51,
      67,    67,    44,    67,    19,    59,    18,    66,    43,    17,
      20,    61,    59,   202,    16,    68,    67,    17,    21,   148,
      61,   116,    68,    84,   133,    54,    -1,   214,    76
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     3,     5,     8,     9,    10,    13,    23,    24,    25,
      32,    33,    34,    35,    36,    37,    41,    58,    76,    77,
      78,    79,    80,    81,    82,    83,    84,     4,    29,     6,
       7,    29,     7,    29,    59,   120,    11,    14,   120,    39,
      40,    50,   118,    45,    46,    47,    48,    49,    59,    74,
      94,   101,   102,   103,   105,   106,   120,   121,    42,     0,
      58,    64,    14,   120,   120,   120,   120,   120,   120,    24,
      51,    65,    66,    18,   104,    14,    68,   104,    66,    70,
      84,   120,    66,    66,    66,    12,    22,    92,    59,    98,
      99,   121,    52,    63,    74,    94,   121,    66,   108,   110,
     120,   103,    94,   121,    85,    87,   121,    86,   121,    86,
      66,    91,    93,    94,   105,   107,    68,    92,    65,    53,
      54,   119,    67,    67,    84,   109,    31,    68,    92,    18,
      59,    67,    67,    68,    26,    27,    28,    88,    67,    68,
      67,    60,    61,    62,    63,    69,    89,    90,    30,    55,
      56,    57,    65,    71,    72,    96,    99,    90,    94,   100,
      51,    44,    44,    67,   108,   108,    19,   111,    59,    87,
      66,   121,    61,    62,    67,    68,    91,    90,    94,    97,
     105,    69,    73,    84,    84,    18,    43,    17,    20,   112,
      61,    90,    90,    90,    59,    93,    94,    95,    93,    16,
     113,    67,    68,    17,    21,   117,    94,   107,   114,   115,
      61,     9,    15,   116,    68,   115
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_int8 yyr1[] =
{
       0,    75,    76,    76,    76,    76,    76,    77,    77,    77,
      77,    77,    77,    78,    79,    79,    79,    79,    80,    80,
      81,    81,    82,    82,    82,    82,    82,    82,    83,    83,
      83,    83,    84,    85,    85,    86,    86,    87,    88,    88,
      88,    89,    89,    90,    90,    90,    90,    90,    90,    91,
      92,    92,    93,    93,    94,    94,    95,    95,    96,    96,
      96,    96,    96,    96,    97,    97,    97,    98,    98,    99,
     100,   100,   100,   100,   101,   101,   102,   102,   103,   103,
     104,   104,   105,   105,   105,   106,   106,   106,   106,   107,
     107,   108,   108,   108,   108,   109,   109,   110,   110,   110,
     110,   111,   111,   112,   112,   113,   113,   114,   114,   115,
     116,   116,   116,   117,   117,   118,   118,   119,   119,   120,
     121
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     2,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     3,     1,     1,     1,     1,     2,     4,
       4,     5,     6,     2,     3,     2,     6,     6,     7,     4,
       5,     1,     9,     1,     3,     1,     3,     2,     1,     4,
       1,     1,     3,     1,     2,     1,     2,     1,     1,     3,
       0,     2,     1,     3,     3,     1,     1,     3,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     3,     3,
       1,     1,     3,     3,     1,     1,     1,     3,     2,     2,
       0,     2,     4,     4,     4,     1,     1,     1,     1,     1,
       1,     1,     2,     3,     5,     3,     3,     1,     3,     3,
       5,     0,     3,     0,     2,     3,     0,     1,     3,     2,
       1,     1,     0,     0,     2,     1,     1,     2,     1,     1,
       1
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, scanner, parse_result, error_msg, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Error token number */
#define YYTERROR        1
#define YYERRCODE       256


/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YY_LOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

#ifndef YY_LOCATION_PRINT
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
 }

#  define YY_LOCATION_PRINT(File, Loc)          \
  yy_location_print_ (File, &(Loc))

# else
#  define YY_LOCATION_PRINT(File, Loc) ((void) 0)
# endif
#endif


# define YY_SYMBOL_PRINT(Title, Type, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Type, Value, Location, scanner, parse_result, error_msg); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg)
{
  FILE *yyoutput = yyo;
  YYUSE (yyoutput);
  YYUSE (yylocationp);
  YYUSE (scanner);
  YYUSE (parse_result);
  YYUSE (error_msg);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yytype], *yyvaluep);
# endif
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg)
{
  YYFPRINTF (yyo, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  YY_LOCATION_PRINT (yyo, *yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yytype, yyvaluep, yylocationp, scanner, parse_result, error_msg);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp, int yyrule, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       yystos[+yyssp[yyi + 1 - yynrhs]],
                       &yyvsp[(yyi + 1) - (yynrhs)]
                       , &(yylsp[(yyi + 1) - (yynrhs)])                       , scanner, parse_result, error_msg);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule, scanner, parse_result, error_msg); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
#  else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
# endif

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return 1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return 2 if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                yy_state_t *yyssp, int yytoken)
{
  enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
  /* Actual size of YYARG. */
  int yycount = 0;
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yytoken != YYEMPTY)
    {
      int yyn = yypact[+*yyssp];
      YYPTRDIFF_T yysize0 = yytnamerr (YY_NULLPTR, yytname[yytoken]);
      yysize = yysize0;
      yyarg[yycount++] = yytname[yytoken];
      if (!yypact_value_is_default (yyn))
        {
          /* Start YYX at -YYN if negative to avoid negative indexes in
             YYCHECK.  In other words, skip the first -YYN actions for
             this state because they are default actions.  */
          int yyxbegin = yyn < 0 ? -yyn : 0;
          /* Stay within bounds of both yycheck and yytname.  */
          int yychecklim = YYLAST - yyn + 1;
          int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
          int yyx;

          for (yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR
                && !yytable_value_is_error (yytable[yyx + yyn]))
              {
                if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                  {
                    yycount = 1;
                    yysize = yysize0;
                    break;
                  }
                yyarg[yycount++] = yytname[yyx];
                {
                  YYPTRDIFF_T yysize1
                    = yysize + yytnamerr (YY_NULLPTR, yytname[yyx]);
                  if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
                    yysize = yysize1;
                  else
                    return 2;
                }
              }
        }
    }

  switch (yycount)
    {
# define YYCASE_(N, S)                      \
      case N:                               \
        yyformat = S;                       \
      break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
# undef YYCASE_
    }

  {
    /* Don't count the "%s"s in the final size, but reserve room for
       the terminator.  */
    YYPTRDIFF_T yysize1 = yysize + (yystrlen (yyformat) - 2 * yycount) + 1;
    if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
      yysize = yysize1;
    else
      return 2;
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return 1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yyarg[yyi++]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}
#endif /* YYERROR_VERBOSE */

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg)
{
  YYUSE (yyvaluep);
  YYUSE (yylocationp);
  YYUSE (scanner);
  YYUSE (parse_result);
  YYUSE (error_msg);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}




/*----------.
| yyparse.  |
`----------*/

int
yyparse (yyscan_t scanner, std::shared_ptr<ast::TreeNode> *parse_result, std::string *error_msg)
{
/* The lookahead symbol.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs;

    yy_state_fast_t yystate;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus;

    /* The stacks and their tools:
       'yyss': related to states.
       'yyvs': related to semantic values.
       'yyls': related to locations.

       Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* The state stack.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss;
    yy_state_t *yyssp;

    /* The semantic value stack.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs;
    YYSTYPE *yyvsp;

    /* The location stack.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls;
    YYLTYPE *yylsp;

    /* The locations where the error started and ended.  */
    YYLTYPE yyerror_range[3];

    YYPTRDIFF_T yystacksize;

  int yyn;
  int yyresult;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;
#endif

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  yyssp = yyss = yyssa;
  yyvsp = yyvs = yyvsa;
  yylsp = yyls = yylsa;
  yystacksize = YYINITDEPTH;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY; /* Cause a token to be read.  */
  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    goto yyexhaustedlab;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
# undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = yylex (&yylval, &yylloc, scanner);
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2:
#line 88 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        *parse_result = (yyvsp[-1].sv_node);
        YYACCEPT;
    }
#line 1676 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 3:
#line 93 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        *parse_result = (yyvsp[-1].sv_node);
        YYACCEPT;
    }
#line 1685 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 4:
#line 98 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        *parse_result = std::make_shared<Help>();
        YYACCEPT;
    }
#line 1694 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 5:
#line 103 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
#line 1703 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 6:
#line 108 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        *parse_result = nullptr;
        YYACCEPT;
    }
#line 1712 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 13:
#line 125 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // 任务四入口：EXPLAIN ANALYZE 只解释 SELECT，不直接输出查询结果
        (yyval.sv_node) = std::make_shared<ExplainAnalyzeStmt>(std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node)));
    }
#line 1721 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 14:
#line 133 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnBegin>();
    }
#line 1729 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 15:
#line 137 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnCommit>();
    }
#line 1737 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 16:
#line 141 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnAbort>();
    }
#line 1745 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 17:
#line 145 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnRollback>();
    }
#line 1753 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 18:
#line 152 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowTables>();
    }
#line 1761 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 19:
#line 156 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowIndex>((yyvsp[0].sv_str));
    }
#line 1769 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 20:
#line 163 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetStmt>((yyvsp[-2].sv_setKnobType), (yyvsp[0].sv_bool));
    }
#line 1777 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 21:
#line 167 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetTransactionIsolationStmt>((yyvsp[0].sv_isolation_level));
    }
#line 1785 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 22:
#line 174 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateTable>((yyvsp[-3].sv_str), (yyvsp[-1].sv_fields));
    }
#line 1793 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 23:
#line 178 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateStaticCheckpoint>();
    }
#line 1801 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 24:
#line 182 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropTable>((yyvsp[0].sv_str));
    }
#line 1809 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 25:
#line 186 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DescTable>((yyvsp[0].sv_str));
    }
#line 1817 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 26:
#line 190 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1825 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 27:
#line 194 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1833 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 28:
#line 201 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<InsertStmt>((yyvsp[-4].sv_str), (yyvsp[-1].sv_vals));
    }
#line 1841 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 29:
#line 205 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DeleteStmt>((yyvsp[-1].sv_str), (yyvsp[0].sv_conds));
    }
#line 1849 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 30:
#line 209 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<UpdateStmt>((yyvsp[-3].sv_str), (yyvsp[-1].sv_set_clauses), (yyvsp[0].sv_conds));
    }
#line 1857 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 32:
#line 217 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // FROM 子句保留真实表名、别名和 JOIN ON 条件，后续 Analyze/Planner 再做下推优化
        (yyval.sv_node) = std::make_shared<SelectStmt>((yyvsp[-7].sv_cols), (yyvsp[-5].sv_from_clause), (yyvsp[-4].sv_conds), (yyvsp[-3].sv_cols), (yyvsp[-2].sv_conds), (yyvsp[-1].sv_orderbys), (yyvsp[0].sv_int));
    }
#line 1866 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 33:
#line 225 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_fields) = std::vector<std::shared_ptr<Field>>{(yyvsp[0].sv_field)};
    }
#line 1874 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 34:
#line 229 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_fields).push_back((yyvsp[0].sv_field));
    }
#line 1882 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 35:
#line 236 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str)};
    }
#line 1890 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 36:
#line 240 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_strs).push_back((yyvsp[0].sv_str));
    }
#line 1898 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 37:
#line 247 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_field) = std::make_shared<ColDef>((yyvsp[-1].sv_str), (yyvsp[0].sv_type_len));
    }
#line 1906 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 38:
#line 254 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
#line 1914 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 39:
#line 258 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, (yyvsp[-1].sv_int));
    }
#line 1922 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 40:
#line 262 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
#line 1930 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 41:
#line 269 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_vals) = std::vector<std::shared_ptr<ast::Value>>{(yyvsp[0].sv_val)};
    }
#line 1938 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 42:
#line 273 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_vals).push_back((yyvsp[0].sv_val));
    }
#line 1946 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 43:
#line 280 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>((yyvsp[0].sv_int));
    }
#line 1954 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 44:
#line 284 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>(-(yyvsp[0].sv_int));
    }
#line 1962 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 45:
#line 288 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>((yyvsp[0].sv_float));
    }
#line 1970 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 46:
#line 292 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>(-(yyvsp[0].sv_float));
    }
#line 1978 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 47:
#line 296 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<StringLit>((yyvsp[0].sv_str));
    }
#line 1986 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 48:
#line 300 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<BoolLit>((yyvsp[0].sv_bool));
    }
#line 1994 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 49:
#line 307 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_cond) = std::make_shared<BinaryExpr>((yyvsp[-2].sv_col), (yyvsp[-1].sv_comp_op), (yyvsp[0].sv_expr));
    }
#line 2002 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 50:
#line 313 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_conds) = {}; }
#line 2008 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 51:
#line 315 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2016 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 52:
#line 322 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>{(yyvsp[0].sv_cond)};
    }
#line 2024 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 53:
#line 326 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_conds).push_back((yyvsp[0].sv_cond));
    }
#line 2032 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 54:
#line 333 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2040 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 55:
#line 337 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>("", (yyvsp[0].sv_str));
    }
#line 2048 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 56:
#line 344 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)};
    }
#line 2056 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 57:
#line 348 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_cols).push_back((yyvsp[0].sv_col));
    }
#line 2064 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 58:
#line 355 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_EQ;
    }
#line 2072 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 59:
#line 359 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LT;
    }
#line 2080 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 60:
#line 363 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GT;
    }
#line 2088 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 61:
#line 367 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_NE;
    }
#line 2096 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 62:
#line 371 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LE;
    }
#line 2104 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 63:
#line 375 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GE;
    }
#line 2112 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 64:
#line 382 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_val));
    }
#line 2120 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 65:
#line 386 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_col));
    }
#line 2128 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 66:
#line 390 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_col));
    }
#line 2136 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 67:
#line 397 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_set_clauses) = std::vector<std::shared_ptr<ast::SetClause>>{(yyvsp[0].sv_set_clause)};
    }
#line 2144 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 68:
#line 401 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_set_clauses).push_back((yyvsp[0].sv_set_clause));
    }
#line 2152 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 69:
#line 408 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<ast::SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_update_expr));
    }
#line 2160 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 70:
#line 415 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_update_expr) = std::make_shared<ast::UpdateExpr>((yyvsp[0].sv_val));
    }
#line 2168 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 71:
#line 419 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_update_expr) = std::make_shared<ast::UpdateExpr>((yyvsp[0].sv_col));
    }
#line 2176 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 72:
#line 423 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_update_expr) = std::make_shared<ast::UpdateExpr>(ast::UpdateExprKind::ADD,
             std::make_shared<ast::UpdateExpr>((yyvsp[-2].sv_col)), std::make_shared<ast::UpdateExpr>((yyvsp[0].sv_val)));
    }
#line 2185 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 73:
#line 428 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_update_expr) = std::make_shared<ast::UpdateExpr>(ast::UpdateExprKind::SUB,
             std::make_shared<ast::UpdateExpr>((yyvsp[-2].sv_col)), std::make_shared<ast::UpdateExpr>((yyvsp[0].sv_val)));
    }
#line 2194 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 74:
#line 435 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_cols) = {}; }
#line 2200 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 76:
#line 440 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                   { (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)}; }
#line 2206 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 77:
#line 441 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                                      { (yyval.sv_cols).push_back((yyvsp[0].sv_col)); }
#line 2212 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 78:
#line 445 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyvsp[-1].sv_col)->alias = (yyvsp[0].sv_str); (yyval.sv_col) = (yyvsp[-1].sv_col); }
#line 2218 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 79:
#line 446 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                            { (yyvsp[-1].sv_col)->alias = (yyvsp[0].sv_str); (yyval.sv_col) = (yyvsp[-1].sv_col); }
#line 2224 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 80:
#line 450 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_str) = ""; }
#line 2230 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 81:
#line 451 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                   { (yyval.sv_str) = (yyvsp[0].sv_str); }
#line 2236 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 82:
#line 455 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                          { (yyval.sv_col) = std::make_shared<Col>(AGG_COUNT, "", "*", true); }
#line 2242 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 83:
#line 456 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                          { (yyval.sv_col) = std::make_shared<Col>(AGG_COUNT, (yyvsp[-1].sv_col)->tab_name, (yyvsp[-1].sv_col)->col_name, false); }
#line 2248 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 84:
#line 457 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                             { (yyval.sv_col) = std::make_shared<Col>((yyvsp[-3].sv_agg_func), (yyvsp[-1].sv_col)->tab_name, (yyvsp[-1].sv_col)->col_name, false); }
#line 2254 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 85:
#line 461 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_agg_func) = AGG_MAX; }
#line 2260 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 86:
#line 462 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_agg_func) = AGG_MIN; }
#line 2266 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 87:
#line 463 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_agg_func) = AGG_SUM; }
#line 2272 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 88:
#line 464 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_agg_func) = AGG_AVG; }
#line 2278 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 89:
#line 468 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_col) = (yyvsp[0].sv_col); }
#line 2284 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 90:
#line 469 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                  { (yyval.sv_col) = (yyvsp[0].sv_col); }
#line 2290 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 91:
#line 474 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // 无别名时，输出和解析都继续使用真实表名
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[0].sv_str), "");
    }
#line 2299 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 92:
#line 479 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // 支持 FROM customers c 这种任务四常用表别名写法
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-1].sv_str), (yyvsp[0].sv_str));
    }
#line 2308 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 93:
#line 484 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2316 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 94:
#line 488 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-3].sv_union), (yyvsp[0].sv_str));
    }
#line 2324 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 95:
#line 495 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_union) = std::make_shared<UnionStmt>(std::dynamic_pointer_cast<SelectStmt>((yyvsp[-2].sv_node)),
                                         std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node)));
    }
#line 2333 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 96:
#line 500 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyvsp[-2].sv_union)->branches.push_back(std::dynamic_pointer_cast<SelectStmt>((yyvsp[0].sv_node)));
        (yyval.sv_union) = (yyvsp[-2].sv_union);
    }
#line 2342 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 97:
#line 508 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        (yyval.sv_from_clause) = std::make_shared<FromClause>((yyvsp[0].sv_table_ref));
    }
#line 2350 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 98:
#line 512 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // 兼容原有逗号分隔多表查询
        (yyvsp[-2].sv_from_clause)->tables.push_back((yyvsp[0].sv_table_ref));
        (yyval.sv_from_clause) = (yyvsp[-2].sv_from_clause);
    }
#line 2360 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 99:
#line 518 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // 兼容原有无 ON 的 JOIN，语义等价于没有连接条件的多表连接
        (yyvsp[-2].sv_from_clause)->tables.push_back((yyvsp[0].sv_table_ref));
        (yyval.sv_from_clause) = (yyvsp[-2].sv_from_clause);
    }
#line 2370 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 100:
#line 524 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
    {
        // JOIN ON 条件单独保存，后续计划生成时应进入 Join 节点
        (yyvsp[-4].sv_from_clause)->tables.push_back((yyvsp[-2].sv_table_ref));
        (yyvsp[-4].sv_from_clause)->join_conds.insert((yyvsp[-4].sv_from_clause)->join_conds.end(), (yyvsp[0].sv_conds).begin(), (yyvsp[0].sv_conds).end());
        (yyval.sv_from_clause) = (yyvsp[-4].sv_from_clause);
    }
#line 2381 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 101:
#line 533 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_cols) = {}; }
#line 2387 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 102:
#line 534 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                         { (yyval.sv_cols) = (yyvsp[0].sv_cols); }
#line 2393 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 103:
#line 538 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_conds) = {}; }
#line 2399 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 104:
#line 539 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                           { (yyval.sv_conds) = (yyvsp[0].sv_conds); }
#line 2405 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 105:
#line 543 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                              { (yyval.sv_orderbys) = (yyvsp[0].sv_orderbys); }
#line 2411 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 106:
#line 544 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_orderbys) = {}; }
#line 2417 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 107:
#line 548 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                   { (yyval.sv_orderbys) = std::vector<std::shared_ptr<OrderBy>>{(yyvsp[0].sv_orderby)}; }
#line 2423 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 108:
#line 549 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                                    { (yyval.sv_orderbys).push_back((yyvsp[0].sv_orderby)); }
#line 2429 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 109:
#line 553 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                                   { (yyval.sv_orderby) = std::make_shared<OrderBy>((yyvsp[-1].sv_col), (yyvsp[0].sv_orderby_dir)); }
#line 2435 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 110:
#line 557 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_ASC;     }
#line 2441 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 111:
#line 558 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_DESC;    }
#line 2447 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 112:
#line 559 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
            { (yyval.sv_orderby_dir) = OrderBy_DEFAULT; }
#line 2453 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 113:
#line 563 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                      { (yyval.sv_int) = -1; }
#line 2459 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 114:
#line 564 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                        { (yyval.sv_int) = (yyvsp[0].sv_int); }
#line 2465 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 115:
#line 568 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                    { (yyval.sv_setKnobType) = EnableNestLoop; }
#line 2471 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 116:
#line 569 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                         { (yyval.sv_setKnobType) = EnableSortMerge; }
#line 2477 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 117:
#line 573 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                       { (yyval.sv_isolation_level) = TxnIsolationLevel::SNAPSHOT_ISOLATION; }
#line 2483 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;

  case 118:
#line 574 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"
                     { (yyval.sv_isolation_level) = TxnIsolationLevel::SERIALIZABLE; }
#line 2489 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"
    break;


#line 2493 "/home/lime/Documents/db2026-qry/src/parser/yacc.tab.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYEMPTY : YYTRANSLATE (yychar);

  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (&yylloc, scanner, parse_result, error_msg, YY_("syntax error"));
#else
# define YYSYNTAX_ERROR yysyntax_error (&yymsg_alloc, &yymsg, \
                                        yyssp, yytoken)
      {
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = YYSYNTAX_ERROR;
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == 1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *, YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (!yymsg)
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = 2;
              }
            else
              {
                yysyntax_error_status = YYSYNTAX_ERROR;
                yymsgp = yymsg;
              }
          }
        yyerror (&yylloc, scanner, parse_result, error_msg, yymsgp);
        if (yysyntax_error_status == 2)
          goto yyexhaustedlab;
      }
# undef YYSYNTAX_ERROR
#endif
    }

  yyerror_range[1] = yylloc;

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc, scanner, parse_result, error_msg);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYTERROR;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  yystos[yystate], yyvsp, yylsp, scanner, parse_result, error_msg);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  /* Using YYLLOC is tempting, but would change the location of
     the lookahead.  YYLOC is available though.  */
  YYLLOC_DEFAULT (yyloc, yyerror_range, 2);
  *++yylsp = yyloc;

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;


#if !defined yyoverflow || YYERROR_VERBOSE
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, scanner, parse_result, error_msg, YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif


/*-----------------------------------------------------.
| yyreturn -- parsing is finished, return the result.  |
`-----------------------------------------------------*/
yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc, scanner, parse_result, error_msg);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  yystos[+*yyssp], yyvsp, yylsp, scanner, parse_result, error_msg);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  return yyresult;
}
#line 580 "/home/lime/Documents/db2026-qry/src/parser/yacc.y"

