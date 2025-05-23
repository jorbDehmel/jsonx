/*
 * Command line Calculator using libBF
 * 
 * Copyright (c) 2008-2025 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

#include "list.h"
#include "cutils.h"
#include "libbf.h"
#include "readline_tty.h"

/*
  TODO:
  - readline > 80 cols
  - file as input
  - solver
  - add CTYPE_TYPE to be able to represent types ?
  - polynomials: add an optional variable name and support polynomial coefficients ?
  - rfrac: convert fractional poly to integer ones
  - fix tensor init to boolean
*/

typedef struct BCContext BCContext;
typedef struct BCValueStruct *BCValue;
typedef const struct BCValueStruct *BCValueConst;
typedef struct BCType BCType;

/* all values are immutable except tensors and arrays */
typedef enum {
    /* warning: the ordering of the type values matters */
    CTYPE_BOOL = 0, /* boolean */
    CTYPE_INT,  /* integers (signed) */
    CTYPE_FRAC, /* fraction of integers */
    CTYPE_DECIMAL, /* decimal floating point number */
    CTYPE_FLOAT, /* binary floating point number */
    CTYPE_COMPLEX, /* complex number. elem_type = int, frac, decimal, float */

    CTYPE_POLY,  /* polynomial. elem_type = int, frac, decimal, float, complex */
    CTYPE_RFRAC, /* rational fonction */
    CTYPE_SER,   /* power series */
    CTYPE_TENSOR, /* multi-dimension array. Same type for all elements */

    CTYPE_ARRAY, /* single dimension growable array. Each element has
                    any type */
    CTYPE_FUNCTION,
    CTYPE_STRING,
    CTYPE_NULL,
    CTYPE_RANGE,
    
    CTYPE_COUNT, /* number of types */
} BCTypeEnum;

static __maybe_unused const char *ctype_str[CTYPE_COUNT] = 
    { "Boolean", "Integer", "Fraction", "Decimal", "Float", "Complex", "Polynomial", "RationalFunction", "Series", "Tensor", "Array", "Function", "String", "Null", "Range" };

typedef enum {
    CERR_TYPE,
    CERR_RANGE,
    CERR_SYNTAX,
    CERR_REFERENCE,

    CERR_COUNT, /* number of errors */
} BCErrorTypeEnum;

static const char *cerr_type_str[CERR_COUNT] = {
  "TypeError",
  "RangeError",
  "SyntaxError",
  "ReferenceError",
};

#define FUNCTION_MAX_ARGS 3

typedef BCValue Map1Func(BCContext *ctx, BCValue v1, void *opaque);

typedef BCValue UnaryFunc(BCContext *ctx, BCValue v1);

typedef struct {
    int nb_args; /* 0 ... FUNCTION_MAX_ARGS */
    BOOL var_args; /* if variable numbers of args, at least
                      nb_args must be provided */
    union {
        void *cfunc;
        BCValue (*cfunc0)(BCContext *ctx);
        BCValue (*cfunc1)(BCContext *ctx, BCValue arg0);
        BCValue (*cfunc2)(BCContext *ctx, BCValue arg0, BCValue arg1);
        BCValue (*cfunc3)(BCContext *ctx, BCValue arg0, BCValue arg1, BCValue arg2);
        BCValue (*cfunc_vararg)(BCContext *ctx, int n_args, BCValue *args);
    } u;
    char *name;
} BCFunction;

typedef struct {
    int len; /* >= 1 for polynomials. Can be zero for series */
    int emin; /* always 0 for polynomials */
    BCValue *tab;
} BCPoly;

#define MAX_DIMS 4

typedef struct {
    int n_dims; /* 1-MAX_DIMS */
    int dims[MAX_DIMS];
    int size; /* product of dimensions */
    BCValue *tab; /* size elements */
} BCTensor;

typedef struct {
    int len;
    int size;
    BCValue *tab; /* size elements */
} BCArray;

#define RANGE_DEFAULT INT32_MIN

typedef struct {
    int start; /* RANGE_DEFAULT is allowed */
    int stop; /* RANGE_DEFAULT is allowed */
} BCRange;

typedef struct {
    int len;
    uint8_t *str; /* zero terminated */
} BCString;

struct BCType {
    int ref_count;
    BCTypeEnum tag;
    BCType *elem_type;
};

static BOOL has_elem_type(BCTypeEnum tag)
{
    return (tag == CTYPE_COMPLEX || tag == CTYPE_POLY ||
            tag == CTYPE_RFRAC || tag == CTYPE_SER ||
            tag == CTYPE_TENSOR);
}

struct BCValueStruct {
    int ref_count;
    BCType *type;
    union {
        bf_t cint;
        bf_t cfloat;
        bfdec_t cdec;
        struct {
            BCValue num; /* integer */
            BCValue den; /* integer */
        } frac;
        struct {
            BCValue re;
            BCValue im;
        } complex;
        BCFunction function;
        BCTensor tensor;
        BCArray array;
        BOOL bool_val;
        BCRange range;
        BCString string;
        BCPoly poly;
        struct {
            BCValue num; /* polynomial */
            BCValue den; /* polynomial */
        } rfrac;
    } u;
};

/* special BCValue to indicate an error */
#define BC_EXCEPTION NULL

typedef enum {
    BC_OP2_ADD,
    BC_OP2_SUB,
    BC_OP2_MUL,
    BC_OP2_DIV, /* return a decimal when dividing integers */
    BC_OP2_MOD,
    BC_OP2_POW,
    BC_OP2_ATAN2,
    BC_OP2_DOT_MUL,
    BC_OP2_CMP_EQ,
    BC_OP2_CMP_LT,
    BC_OP2_CMP_LE,
    BC_OP2_OR,
    BC_OP2_AND,
    BC_OP2_XOR,
    BC_OP2_DIVREM, /* return [q, r] */
    BC_OP2_FRAC_DIV, /* return a fraction when dividing integers */
} BCOP2Enum;

typedef enum {
    BC_OP1_NEG,
    BC_OP1_ABS,
    BC_OP1_TRUNC,
    BC_OP1_FLOOR,
    BC_OP1_CEIL,
    BC_OP1_ROUND,
    BC_OP1_CONJ,
    BC_OP1_RE,
    BC_OP1_IM,
    BC_OP1_SQRT,
    BC_OP1_EXP,
    BC_OP1_LOG,
    BC_OP1_SIN,
    BC_OP1_COS,
    BC_OP1_TAN,
    BC_OP1_ASIN,
    BC_OP1_ACOS,
    BC_OP1_ATAN,
} BCOP1Enum;

typedef enum {
    BC_CONST_PI,
    BC_CONST_LOG2E,
    BC_CONST_LOG10E,
    BC_CONST_LOG10,

    BC_CONST_COUNT,
} BCConstEnum;

typedef struct {
    BCValue value; /* CTYPE_DECIMAL */
    int prec; /* 0 if none */
} BCConstDef;

typedef struct {
    struct list_head link;
    char *name;
    BCValue value;
    BOOL is_user : 8; /* true if defined by the user */
    BOOL is_getter : 8; /* function call when accessing the variable */
} BCVarDef;

struct BCContext {
    bf_context_t bf_ctx;
    /* if has_error is true, an error is pending */
    BOOL has_error;
    BCErrorTypeEnum error_type;
    char error_msg[64];

    /* preallocated types and values */
    BCType *def_type[CTYPE_COUNT];
    BCValue null_value;
    BCValue bool_value[2];

    BCConstDef const_tab[BC_CONST_COUNT];
    
    struct list_head var_list; /* list of BCVarDef.link */

    /* current mode */
    BOOL hex_output : 8; /* output integers and binary floats in hexa */
    BOOL js_mode : 8; /* javascript mode: '[]' is an array literal instead of a tensor, '^' is xor instead of power */
    BOOL tensor_output_lf: 8; /* no line feed in tensor output */
    int float_prec; /* binary float precision */
    int float_flags; /* binary float flags (including exponent size) */
    int dec_prec; /* decimal float precision */
    int dec_flags; /* decimal float flags (including exponent size) */
};

enum {
    TOK_NUMBER = 256, 
    TOK_IDENT,
    TOK_STRING,
    TOK_NULL,
    TOK_TRUE,
    TOK_FALSE,
    TOK_EOF,
    TOK_SHL,
    TOK_LTE,
    TOK_SAR,
    TOK_GTE,
    TOK_STRICT_EQ,
    TOK_EQ,
    TOK_STRICT_NEQ,
    TOK_NEQ,
    TOK_POW,
    TOK_XOR,
    TOK_DIV2,
    TOK_DOT_MUL,
    TOK_POW_ASSIGN,
    TOK_MUL_ASSIGN,
    TOK_DIV_ASSIGN,
};

#define IDENT_SIZE_MAX 128

typedef struct {
    int val;
    BCValue value;
    char ident[IDENT_SIZE_MAX];
} Token;

typedef struct {
    BCContext *ctx;
    const char *buf_ptr;
    Token token; /* current token */
    /* error handling */
    jmp_buf jmp_env;
    BCErrorTypeEnum error_type;
    char error_msg[64];
} ParseState;

static void cval_toString(BCContext *ctx, DynBuf *d, BCValueConst v1);
static void cval_dump(BCContext *ctx, const char *str, BCValueConst val);
static BCValue cval_add(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_sub(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_mul(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_div(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_mod(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_pow(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_neg(BCContext *ctx, BCValue v1);
static BCValue cval_abs(BCContext *ctx, BCValue v1);
static BCValue cval_cmp_eq(BCContext *ctx, BCValue v1, BCValue v2);
static BOOL cval_cmp_eq2(BCContext *ctx, BCValueConst v1, BCValueConst v2);
static BOOL cval_cmp_lt2(BCContext *ctx, BCValueConst v1, BCValueConst v2);
static BOOL cval_cmp_le2(BCContext *ctx, BCValueConst v1, BCValueConst v2);
static BOOL cval_cmp_eq_int(BCContext *ctx, BCValueConst a, int b);
static BOOL cval_cmp_lt_int(BCContext *ctx, BCValueConst a, int b);
static BCValue cval_convert(BCContext *ctx, BCValue v1, const BCType *t1);
static BCValue cval_sqrt(BCContext *ctx, BCValue v1);
static BCValue cval_exp(BCContext *ctx, BCValue v1);
static BCValue cval_sin(BCContext *ctx, BCValue v1);
static BCValue cval_cos(BCContext *ctx, BCValue v1);
static BCValue cval_atan2(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_log(BCContext *ctx, BCValue v1);
static BCValue cval_op1(BCContext *ctx, BCValue v1, BCOP1Enum op);
static BCValue cval_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op);
static BCValue cval_floor(BCContext *ctx, BCValue v1);
static BCValue matrix_mul(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue cval_inverse(BCContext *ctx, BCValue v1);
static BCValue cval_trunc(BCContext *ctx, BCValue v1);
static BCValue cval_and(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue carray_pair(BCContext *ctx, BCValue q, BCValue r);
static BCValue cval_round(BCContext *ctx, BCValue v1);
static BCValue cval_divexact(BCContext *ctx,  BCValue v1, BCValue v2);
static BCValue cfrac_new(BCContext *ctx, BCValue num, BCValue den);
static BCValue cval_gcd(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue rfrac_new(BCContext *ctx, BCValue num, BCValue den);
static BCValue cval_deriv(BCContext *ctx, BCValue v1);
static BCValue cval_integ(BCContext *ctx, BCValue v1);
static BCValue cval_shl(BCContext *ctx, BCValue v1, BCValue v2);
static BCValue carray_new(BCContext *ctx, int allocated_len);
static int carray_push1(BCContext *ctx, BCValueConst tab, BCValue a);
static BCValue poly_new(BCContext *ctx, const BCType *elem_type, int len);
static BCValue cval_norm2(BCContext *ctx, BCValue v1);
static BCValue poly_roots(BCContext *ctx, int n_args, BCValue *args);

static void *my_bf_realloc(void *opaque, void *ptr, size_t size)
{
    return realloc(ptr, size);
}

void *mallocz(size_t size)
{
    void *ptr;
    ptr = malloc(size);
    if (!ptr)
        return NULL;
    memset(ptr, 0, size);
    return ptr;
}

/* types */

static BCType *ctype_dup(const BCType *t1)
{
    BCType *t = (BCType *)t1;
    t->ref_count++;
    return t;
}

static void ctype_free(BCContext *ctx, BCType *type);

static void __ctype_free(BCContext *ctx, BCType *t)
{
    if (t->elem_type)
        ctype_free(ctx, t->elem_type);
    free(t);
}

static void ctype_free(BCContext *ctx, BCType *t)
{
    assert(t->ref_count >= 1);
    if (--t->ref_count == 0)
        __ctype_free(ctx, t);
}

static BCType *ctype_new(BCContext *ctx, BCTypeEnum tag,
                         const BCType *elem_type)
{
    BCType *t;
    t = malloc(sizeof(*t));
    t->ref_count = 1;
    t->tag = tag;
    if (elem_type)
        t->elem_type = ctype_dup(elem_type);
    else
        t->elem_type = NULL;
    return t;
}

static BCType *ctype_new_free(BCContext *ctx, BCTypeEnum tag,
                              BCType *elem_type)
{
    BCType *t;
    t = ctype_new(ctx, tag, elem_type);
    if (elem_type)
        ctype_free(ctx, elem_type);
    return t;
}

static BCValue cval_new1(BCContext *ctx, const BCType *type)
{
    BCValue v = malloc(sizeof(*v));
    v->ref_count = 1;
    v->type = ctype_dup(type);
    return v;
}

static BCValue cval_new(BCContext *ctx, BCTypeEnum type_tag)
{
    return cval_new1(ctx, ctx->def_type[type_tag]);
}

static BCValue cval_dup(BCValueConst v)
{
    BCValue v1 = (BCValue )v;
    v1->ref_count++;
    return v1;
}

static inline BCTypeEnum cval_type(BCValueConst v)
{
    return v->type->tag;
}

static void cval_free(BCContext *ctx, BCValue v);

static void __cval_free(BCContext *ctx, BCValue v)
{
    switch(cval_type(v)) {
    case CTYPE_INT:
        bf_delete(&v->u.cint);
        break;
    case CTYPE_FRAC:
        cval_free(ctx, v->u.frac.num);
        cval_free(ctx, v->u.frac.den);
        break;
    case CTYPE_DECIMAL:
        bfdec_delete(&v->u.cdec);
        break;
    case CTYPE_FLOAT:
        bf_delete(&v->u.cfloat);
        break;
    case CTYPE_COMPLEX:
        cval_free(ctx, v->u.complex.re);
        cval_free(ctx, v->u.complex.im);
        break;
    case CTYPE_NULL:
    case CTYPE_BOOL:
    case CTYPE_RANGE:
        break;
    case CTYPE_FUNCTION:
        free(v->u.function.name);
        break;
    case CTYPE_ARRAY:
        {
            BCArray *arr = &v->u.array;
            int i;
            for(i = 0; i < arr->len; i++)
                cval_free(ctx, arr->tab[i]);
            free(arr->tab);
        }
        break;
    case CTYPE_TENSOR:
        {
            BCTensor *tr = &v->u.tensor;
            int i;
            for(i = 0; i < tr->size; i++)
                cval_free(ctx, tr->tab[i]);
            free(tr->tab);
        }
        break;
    case CTYPE_POLY:
    case CTYPE_SER:
        {
            BCPoly *p = &v->u.poly;
            int i;
            for(i = 0; i < p->len; i++)
                cval_free(ctx, p->tab[i]);
            free(p->tab);
        }
        break;
    case CTYPE_RFRAC:
        cval_free(ctx, v->u.rfrac.num);
        cval_free(ctx, v->u.rfrac.den);
        break;
    case CTYPE_STRING:
        free(v->u.string.str);
        break;
    default:
        abort();
    }
    ctype_free(ctx, v->type);
    free(v);
}

static void cval_free(BCContext *ctx, BCValue v)
{
    if (!v)
        return;
    assert(v->ref_count >= 1);
    if (--v->ref_count == 0) {
        __cval_free(ctx, v);
    }
}

/*************************************************/
/* error */

static BOOL cval_is_error(BCValueConst v)
{
    return (v == NULL);
}

static BCValue cval_throw_error_buf(BCContext *ctx, BCErrorTypeEnum error_type,
                                     const char *msg)
{
    ctx->has_error = TRUE;
    ctx->error_type = error_type;
    pstrcpy(ctx->error_msg, sizeof(ctx->error_msg), msg);
    return BC_EXCEPTION;
}

static __attribute__((format(printf, 3, 4))) BCValue cval_throw_error(BCContext *ctx, BCErrorTypeEnum error_type, const char *fmt, ...)
{
    va_list ap;
    char error_msg[256];
    va_start(ap, fmt);
    vsnprintf(error_msg, sizeof(error_msg), fmt, ap);
    va_end(ap);
    return cval_throw_error_buf(ctx, error_type, error_msg);
}

#define cval_type_error(ctx, fmt, ...) cval_throw_error(ctx, CERR_TYPE, fmt, ##__VA_ARGS__)
#define cval_range_error(ctx, fmt, ...) cval_throw_error(ctx, CERR_RANGE, fmt, ##__VA_ARGS__)
#define cval_syntax_error(ctx, fmt, ...) cval_throw_error(ctx, CERR_SYNTAX, fmt, ##__VA_ARGS__)

static void bc_get_error(BCContext *ctx, char *buf, int buf_size)
{
    if (ctx->has_error) {
        snprintf(buf, buf_size, "%s: %s", cerr_type_str[ctx->error_type],
                 ctx->error_msg);
        ctx->has_error = FALSE;
    } else {
        snprintf(buf, buf_size, "No error");
    }
}

/*************************************************/
/* null */

static BCValue cnull_new(BCContext *ctx)
{
    return cval_dup(ctx->null_value);
}

static __maybe_unused BOOL cmp_null(BCValueConst c)
{
    return (c->type->tag == CTYPE_NULL);
}

/*************************************************/
/* boolean */

static BCValue cbool_new(BCContext *ctx, int val)
{
    return cval_dup(ctx->bool_value[val != 0]);
}

static int cbool_to_int(BCValueConst v)
{
    assert(cval_type(v) == CTYPE_BOOL);
    return v->u.bool_val;
}

/*************************************************/
/* integer */

static BCValue cint_new(BCContext *ctx)
{
    BCValue v;
    v = cval_new(ctx, CTYPE_INT);
    bf_init(&ctx->bf_ctx, &v->u.cint);
    return v;
}

static BCValue cint_from_int(BCContext *ctx, int64_t n)
{
    BCValue v;
    v = cint_new(ctx);
    bf_set_si(&v->u.cint, n);
    return v;
}

static int cint_to_int(BCContext *ctx, int *pres, BCValueConst v)
{
    if (cval_type(v) != CTYPE_INT) {
        cval_type_error(ctx, "integer expected");
        return -1;
    }
    if (bf_get_int32(pres, &v->u.cint, 0)) {
        cval_range_error(ctx, "integer is too large");
        return -1;
    }
    return 0;
}

/* 'v1' is not freed */
void cint_to_string(BCContext *ctx, DynBuf *d, BCValueConst v1, int radix)
{
    char *str;
    str = bf_ftoa(NULL, &v1->u.cint, radix, 0, BF_RNDZ | BF_FTOA_FORMAT_FRAC |
                  BF_FTOA_ADD_PREFIX | BF_FTOA_JS_QUIRKS);
    dbuf_putstr(d, str);
    bf_free(&ctx->bf_ctx, str);
}

static BCValue cint_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op);

/* convert to integer by truncation */
static BCValue to_cint(BCContext *ctx, BCValue v1)
{
    BCValue v;
    int tag = cval_type(v1);
    switch(tag) {
    case CTYPE_INT:
        return v1;
    case CTYPE_BOOL:
        v = cint_from_int(ctx, cbool_to_int(v1));
        break;
    case CTYPE_FRAC:
        v = cint_op2(ctx, cval_dup(v1->u.frac.num), cval_dup(v1->u.frac.den), BC_OP2_DIV);
        break;
    case CTYPE_DECIMAL:
        v1 = cval_trunc(ctx, v1);
        v = cint_new(ctx);
        bfdec_to_f(&v->u.cint, &v1->u.cdec, BF_PREC_INF, BF_RNDZ);
        break;
    case CTYPE_FLOAT:
        v1 = cval_trunc(ctx, v1);
        v = cint_new(ctx);
        bf_set(&v->u.cint, &v1->u.cfloat);
        break;
    default:
        v = cval_type_error(ctx, "cannot convert to integer");
        break;
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue cint_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    int ret;
    
    v1 = to_cint(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_cint(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    switch(op) {
    case BC_OP2_ADD:
        v = cint_new(ctx);
        bf_add(&v->u.cint, &v1->u.cint, &v2->u.cint, BF_PREC_INF, BF_RNDZ);
        break;
    case BC_OP2_SUB:
        v = cint_new(ctx);
        bf_sub(&v->u.cint, &v1->u.cint, &v2->u.cint, BF_PREC_INF, BF_RNDZ);
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        v = cint_new(ctx);
        bf_mul(&v->u.cint, &v1->u.cint, &v2->u.cint, BF_PREC_INF, BF_RNDZ);
        break;
    case BC_OP2_DIV:
        {
            bf_t r_s, *r = &r_s;
            v = cint_new(ctx);
            bf_init(&ctx->bf_ctx, r);
            ret = bf_divrem(&v->u.cint, r, &v1->u.cint, &v2->u.cint, BF_PREC_INF, BF_RNDZ, BF_RNDZ);
            bf_delete(r);
            if (ret != 0)
                goto div_zero;
        }
        break;
    case BC_OP2_MOD:
        v = cint_new(ctx);
        ret = bf_rem(&v->u.cint, &v1->u.cint, &v2->u.cint, BF_PREC_INF, BF_RNDZ,
                     BF_DIVREM_EUCLIDIAN);
        if (ret != 0) {
        div_zero:
            cval_free(ctx, v);
            v = cval_range_error(ctx, "division by zero");
        }
        break;
    case BC_OP2_POW:
        {
            const bf_t *a, *b;
            a = &v1->u.cint;
            b = &v2->u.cint;
            if (b->sign && !bf_is_zero(b)) {
                /* XXX: could accept -1 or 1 with negative power */
                v = cval_range_error(ctx, "power yields non integer result");
            } else {
                v = cint_new(ctx);
                bf_pow(&v->u.cint, a, b, BF_PREC_INF, BF_RNDZ);
            }
        }
        break;
    case BC_OP2_CMP_EQ:
        v = cbool_new(ctx, bf_cmp_eq(&v1->u.cint, &v2->u.cint));
        break;
    case BC_OP2_CMP_LT:
        v = cbool_new(ctx, bf_cmp_lt(&v1->u.cint, &v2->u.cint));
        break;
    case BC_OP2_CMP_LE:
        v = cbool_new(ctx, bf_cmp_le(&v1->u.cint, &v2->u.cint));
        break;
    case BC_OP2_OR:
        v = cint_new(ctx);
        bf_logic_or(&v->u.cint, &v1->u.cint, &v2->u.cint);
        break;
    case BC_OP2_AND:
        v = cint_new(ctx);
        bf_logic_and(&v->u.cint, &v1->u.cint, &v2->u.cint);
        break;
    case BC_OP2_XOR:
        v = cint_new(ctx);
        bf_logic_xor(&v->u.cint, &v1->u.cint, &v2->u.cint);
        break;
    case BC_OP2_DIVREM:
        {
            BCValue q, r;
            q = cint_new(ctx);
            r = cint_new(ctx);
            ret = bf_divrem(&q->u.cint, &r->u.cint, &v1->u.cint, &v2->u.cint,
                            BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
            if (ret != 0) {
                cval_free(ctx, q);
                cval_free(ctx, r);
                v = cval_range_error(ctx, "division by zero");
            } else {
                v = carray_pair(ctx, q, r);
            }
        }
        break;
    case BC_OP2_FRAC_DIV:
        return cfrac_new(ctx, v1, v2);
    default:
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue cint_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    switch(op) {
    case BC_OP1_NEG:
        v = cint_new(ctx);
        bf_set(&v->u.cint, &v1->u.cint);
        bf_neg(&v->u.cint);
        break;
    case BC_OP1_ABS:
        v = cint_new(ctx);
        bf_set(&v->u.cint, &v1->u.cint);
        v->u.cint.sign = 0;
        break;
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
    default:
        abort();
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue cint_shl(BCContext *ctx, BCValue a, BCValue b)
{
    BCValue v = cint_new(ctx);
    slimb_t v2;
#if LIMB_BITS == 32
    bf_get_int32(&v2, &b->u.cint, 0);
    if (v2 == INT32_MIN)
        v2 = INT32_MIN + 1;
#else
    bf_get_int64(&v2, &b->u.cint, 0);
    if (v2 == INT64_MIN)
        v2 = INT64_MIN + 1;
#endif
    bf_set(&v->u.cint, &a->u.cint);
    bf_mul_2exp(&v->u.cint, v2, BF_PREC_INF, BF_RNDZ);
    if (v2 < 0) {
        bf_rint(&v->u.cint, BF_RNDD);
    }
    cval_free(ctx, a);
    cval_free(ctx, b);
    return v;
}

static BCValue cint_gcd(BCContext *ctx, BCValue a, BCValue b)
{
    if (cval_type(a) != CTYPE_INT || cval_type(b) != CTYPE_INT) {
        cval_free(ctx, a);
        cval_free(ctx, b);
        return cval_type_error(ctx, "integer expected");
    }
    while (!cval_cmp_eq_int(ctx, b, 0)) {
        BCValue r = cint_op2(ctx, a, cval_dup(b), BC_OP2_MOD);
        a = b;
        b = r;
    }
    cval_free(ctx, b);
    return a;
}

/* assume a <= b */
static BCValue fact_rec(BCContext *ctx, int a, int b)
{
    BCValue r;
    int i;
    
    if ((b - a) <= 5) {
        r = cint_from_int(ctx, a);
        for(i = a + 1; i <= b; i++)
            r = cval_mul(ctx, r, cint_from_int(ctx, i));
        return r;
    } else {
        /* to avoid a quadratic running time it is better to
           multiply numbers of similar size */
        i = (a + b) >> 1;
        return cval_mul(ctx, fact_rec(ctx, a, i), fact_rec(ctx, i + 1, b));
    }
}

static BCValue cval_fact1(BCContext *ctx, int n)
{
    if (n <= 1)
        return cint_from_int(ctx, 1);
    else
        return fact_rec(ctx, 1, n);
}

static BCValue cval_fact(BCContext *ctx, BCValue v1)
{
    int ret, n;
    ret = cint_to_int(ctx, &n, v1);
    cval_free(ctx, v1);
    if (ret)
        return BC_EXCEPTION;
    return cval_fact1(ctx, n);
}

static BCValue cval_comb(BCContext *ctx, BCValue n1, BCValue k1)
{
    int ret, n, k;

    ret = cint_to_int(ctx, &n, n1);
    cval_free(ctx, n1);
    if (ret) {
        cval_free(ctx, k1);
        return BC_EXCEPTION;
    }
    ret = cint_to_int(ctx, &k, k1);
    cval_free(ctx, k1);
    if (ret) {
        return BC_EXCEPTION;
    }

    if (k < 0 || k > n)
        return cint_from_int(ctx, 0);
    if (k > n - k)
        k = n - k;
    if (k == 0)
        return cint_from_int(ctx, 1);
    return cval_divexact(ctx, fact_rec(ctx, n - k + 1, n), fact_rec(ctx, 1, k));
}

/* return 0 if OK, -1 if not inverstible or error */
static int bf_invmod(bf_t *r, const bf_t *x, const bf_t *y)
{
    bf_context_t *s = x->ctx;
    bf_t q, u, v, a, c, t;
    int ret;
    
    bf_init(s, &q);
    bf_init(s, &u);
    bf_init(s, &v);
    bf_init(s, &a);
    bf_init(s, &c);
    bf_init(s, &t);
    bf_set(&u, x);
    bf_set(&v, y);
    bf_set_si(&c, 1);
    bf_set_si(&a, 0);
    while (!bf_is_zero(&u)) {
        bf_divrem(&q, &t, &v, &u, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
        bf_set(&v, &u);
        bf_set(&u, &t);
        bf_set(&t, &c);
        bf_mul(&c, &q, &c, BF_PREC_INF, BF_RNDZ);
        bf_sub(&c, &a, &c, BF_PREC_INF, BF_RNDZ);
        bf_set(&a, &t);
    }
    bf_set_si(&t, 1);
    if (!bf_cmp_eq(&v, &t)) {
        bf_set_si(r, 0);
        ret = -1;
    } else {
        bf_divrem(&q, r, &a, y, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
        ret = 0;
    }
    bf_delete(&q);
    bf_delete(&u);
    bf_delete(&v);
    bf_delete(&a);
    bf_delete(&c);
    bf_delete(&t);
    return ret;
}

/* inverse modulo m */
static BCValue cint_invmod(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCValue v;
    if (cval_type(v1) != CTYPE_INT ||  cval_type(v2) != CTYPE_INT) {
        v = cval_type_error(ctx, "cannot convert to integer");
        goto done;
    }
    if (cval_cmp_lt_int(ctx, v2, 1)) {
        v = cval_range_error(ctx, "the modulo must be positive");
        goto done;
    }
    v = cint_new(ctx);
    if (bf_invmod(&v->u.cint, &v1->u.cint, &v2->u.cint)) {
        cval_free(ctx, v);
        v = cval_range_error(ctx, "not invertible");
    }
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue cint_pmod(BCContext *ctx, BCValue a, BCValue b, BCValue m)
{
    BCValue r;
    
    if (cval_type(a) != CTYPE_INT ||
        cval_type(b) != CTYPE_INT ||
        cval_type(m) != CTYPE_INT) {
        r = cval_type_error(ctx, "integer expected");
        goto done;
    }
    if (cval_cmp_lt_int(ctx, m, 1)) {
        r = cval_range_error(ctx, "the modulo must be positive");
        goto done;
    }
    r = cint_from_int(ctx, 1);
    if (!cval_cmp_eq_int(ctx, b, 0)) {
        if (cval_cmp_lt_int(ctx, b, 0)) {
            a = cint_invmod(ctx, a, cval_dup(m));
            if (cval_is_error(a)) {
                r = BC_EXCEPTION;
                goto done;
            }
            b = cval_neg(ctx, b);
        }
        /* XXX: use more efficient algo with shr */
        for(;;) {
            int s;
            bf_get_int32(&s, &b->u.cint, BF_GET_INT_MOD);
            if (s & 1) {
                r = cval_mod(ctx, cval_mul(ctx, r, cval_dup(a)), cval_dup(m));
            }
            b = cint_shl(ctx, b, cint_from_int(ctx, -1));
            if (cval_cmp_eq_int(ctx, b, 0))
                break;
            a = cval_mod(ctx, cval_mul(ctx, cval_dup(a), a), cval_dup(m));
        }
    }
 done:
    cval_free(ctx, a);
    cval_free(ctx, b);
    cval_free(ctx, m);
    return r;
}

/* return floor(log2(v1)) or -1 if v1 <= 0 */
static BCValue cint_ilog2(BCContext *ctx, BCValue v1)
{
    BCValue v;
    bf_t *a;
    slimb_t res;
    
    if (cval_type(v1) != CTYPE_INT) {
        v = cval_type_error(ctx, "integer expected");
        goto done;
    }
    a = &v1->u.cint;
    if (a->sign || a->expn <= 0) {
        res = -1;
    } else {
        res = a->expn - 1;
    }
    v = cint_from_int(ctx, res);
 done:
    cval_free(ctx, v1);
    return v;
}

static int64_t cint_ctz1(BCContext *ctx, BCValueConst v1)
{
    const bf_t *a = &v1->u.cint;
    slimb_t res;
    if (bf_is_zero(a)) {
        res = -1;
    } else {
        res = bf_get_exp_min(a);
    }
    return res;
}

static BCValue cint_ctz(BCContext *ctx, BCValue v1)
{
    BCValue v;
    
    if (cval_type(v1) != CTYPE_INT) {
        v = cval_type_error(ctx, "integer expected");
    } else {
        v = cint_from_int(ctx, cint_ctz1(ctx, v1));
    }
    cval_free(ctx, v1);
    return v;
}

static uint16_t small_primes[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419, 421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499 };

static BOOL miller_rabin_test(BCContext *ctx, BCValueConst n, int t)
{
    int64_t s, i, j;
    BCValue d, r, n1;
    int ret, a;
    
    assert(cval_type(n) == CTYPE_INT);
    d = cval_sub(ctx, cval_dup(n), cint_from_int(ctx, 1));
    s = cint_ctz1(ctx, d);
    d = cval_shl(ctx, d, cint_from_int(ctx, -s));
    t = min_int(t, countof(small_primes));
    n1 = cval_sub(ctx, cval_dup(n), cint_from_int(ctx, 1));
    ret = TRUE;
    for(j = 0; j < t; j++) {
        a = small_primes[j];
        r = cint_pmod(ctx, cint_from_int(ctx, a), cval_dup(d), cval_dup(n));
        if (cval_cmp_eq_int(ctx, r, 1) ||
            cval_cmp_eq2(ctx, r, n1)) {
            cval_free(ctx, r);
            continue;
        }
        for(i = 1; i < s; i++) {
            r = cval_mul(ctx, cval_dup(r), r);
            r = cval_mod(ctx, r, cval_dup(n));
            if (cval_cmp_eq_int(ctx, r, 1)) {
                cval_free(ctx, r);
                ret = FALSE;
                goto done;
            }
            if (cval_cmp_eq2(ctx, r, n1)) {
                cval_free(ctx, r);
                goto next;
            }
        }
        cval_free(ctx, r);
        ret = FALSE; /* n is composite */
        break;
    next: ;
    }
    /* n is probably prime with probability (1-0.5^t) */
 done:
    cval_free(ctx, d);
    cval_free(ctx, n1);
    return ret; 
}

/* return true if b divides a */
static BOOL cint_divide(BCContext *ctx, BCValue a, BCValue b)
{
    BCValue r;
    BOOL res;
    r = cval_mod(ctx, a, b);
    res = cval_cmp_eq_int(ctx, r, 0);
    cval_free(ctx, r);
    return res;
}

static BOOL is_prime(BCContext *ctx, BCValueConst n, int t)
{
    assert(cval_type(n) == CTYPE_INT);
    int i, d;

    if (t == 0)
        t = 64;
    
    if (cval_cmp_lt_int(ctx, n, 2))
        return FALSE;
    for(i = 0; i < countof(small_primes); i++) {
        d = small_primes[i];
        if (cint_divide(ctx, cval_dup(n), cint_from_int(ctx, d)))
            return FALSE;
        if (cval_cmp_lt_int(ctx, n, d * d))
            return TRUE;
    }
    return miller_rabin_test(ctx, n, t);
}

static BCValue cint_isprime(BCContext *ctx, int n_args, BCValue *args)
{
    int i, t, res;
    if (n_args > 2) {
        cval_type_error(ctx, "at most two arguments expected");
        goto fail;
    }
    if (cval_type(args[0]) != CTYPE_INT) {
        cval_type_error(ctx, "integer expected");
        goto fail;
    }
    if (n_args >= 2) {
        if (cint_to_int(ctx, &t, args[1]))
            goto fail;
    } else {
        t = 0;
    }
    res = is_prime(ctx, args[0], t);
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return cbool_new(ctx, res);
 fail:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return BC_EXCEPTION;
}

static BCValue cint_nextprime(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) != CTYPE_INT) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "integer expected");
    }
    for(;;) {
        v1 = cval_add(ctx, v1, cint_from_int(ctx, 1));
        if (is_prime(ctx, v1, 0))
            break;
    }
    return v1;
}

static BCValue cint_factor(BCContext *ctx, BCValue n)
{
    BCValue r;
    int64_t d;
    if (cval_type(n) != CTYPE_INT) {
        cval_free(ctx, n);
        return cval_type_error(ctx, "integer expected");
    }
    if (cval_cmp_lt_int(ctx, n, 1)) {
        cval_free(ctx, n);
        return cval_range_error(ctx, "positive integer expected");
    }

    r = carray_new(ctx, 0);
    
    /* test 2 */
    d = 2;
    for(;;) {
        if (!cint_divide(ctx, cval_dup(n), cint_from_int(ctx, d)))
            break;
        carray_push1(ctx, r, cint_from_int(ctx, d));
        n = cval_divexact(ctx, n, cint_from_int(ctx, d));
    }
    
    /* test odd numbers */
    d = 3;
    while (!cval_cmp_eq_int(ctx, n, 1)) {
        /* test if prime */
        if (is_prime(ctx, n, 0)) {
            carray_push1(ctx, r, cval_dup(n));
            break;
        }
        /* we are sure this is at least one divisor, so one test */
        for(;;) {
            if (cint_divide(ctx, cval_dup(n), cint_from_int(ctx, d)))
                break;
            d += 2;
        }
        for(;;) {
            carray_push1(ctx, r, cint_from_int(ctx, d));
            n = cval_divexact(ctx, n, cint_from_int(ctx, d));
            if (!cint_divide(ctx, cval_dup(n), cint_from_int(ctx, d)))
                break;
        }
    }
    cval_free(ctx, n);
    return r;
}

/*************************************************/
/* range */

static BCValue crange_new(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCValue v;
    int start, stop;
    
    if (cval_type(v1) == CTYPE_NULL) {
        start = RANGE_DEFAULT;
    } else {
        if (cint_to_int(ctx, &start, v1))
            return BC_EXCEPTION;
    }
    if (cval_type(v2) == CTYPE_NULL) {
        stop = RANGE_DEFAULT;
    } else {
        if (cint_to_int(ctx, &stop, v2))
            return BC_EXCEPTION;
    }

    cval_free(ctx, v1);
    cval_free(ctx, v2);
    v = cval_new(ctx, CTYPE_RANGE);
    v->u.range.start = start;
    v->u.range.stop = stop;
    return v;
}

/*************************************************/
/* fraction */

/* no check */
static BCValue cfrac_new2(BCContext *ctx, BCValue num, BCValue den)
{
    BCValue r;
    assert(cval_type(num) == CTYPE_INT && cval_type(den) == CTYPE_INT);
    r = cval_new(ctx, CTYPE_FRAC);
    r->u.frac.num = num;
    r->u.frac.den = den;
    return r;
}

/* return an irreductible fraction (always positive denominator) */
static BCValue cfrac_new(BCContext *ctx, BCValue num, BCValue den)
{
    BCValue g;

    if (cval_type(num) != CTYPE_INT || cval_type(den) != CTYPE_INT) {
        cval_free(ctx, num);
        cval_free(ctx, den);
        return cval_type_error(ctx, "integer expected");
    }

    if (cval_cmp_eq_int(ctx, den, 0)) {
        cval_free(ctx, num);
        cval_free(ctx, den);
        return cval_range_error(ctx, "division by zero");
    } else if (cval_cmp_lt_int(ctx, den, 0)) {
        num = cval_neg(ctx, num);
        den = cval_neg(ctx, den);
    }

    g = cint_gcd(ctx, cval_dup(num), cval_dup(den));
    if (!cval_cmp_eq_int(ctx, g, 1)) {
        num = cint_op2(ctx, num, cval_dup(g), BC_OP2_DIV);
        den = cint_op2(ctx, den, cval_dup(g), BC_OP2_DIV);
    }
    cval_free(ctx, g);
    return cfrac_new2(ctx, num, den);
}

/* num can be integer or fraction */
static BCValue to_cfrac(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) == CTYPE_FRAC) {
        return v1;
    } else if (cval_type(v1) == CTYPE_BOOL) {
        return cfrac_new2(ctx, to_cint(ctx, v1), cint_from_int(ctx, 1));
    } else if (cval_type(v1) == CTYPE_INT) {
        return cfrac_new2(ctx, v1, cint_from_int(ctx, 1));
    } else {
        return cval_type_error(ctx, "integer or fraction expected");
    }
}

/* XXX: optimize gcd use */
static BCValue cfrac_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_cfrac(ctx, v1);
    v2 = to_cfrac(ctx, v2);
    switch(op) {
    case BC_OP2_ADD:
        v = cfrac_new(ctx, cval_add(ctx, cval_mul(ctx, cval_dup(v1->u.frac.num),
                                        cval_dup(v2->u.frac.den)), 
                               cval_mul(ctx, cval_dup(v2->u.frac.num),
                                        cval_dup(v1->u.frac.den))),
                      cval_mul(ctx, cval_dup(v1->u.frac.den),
                               cval_dup(v2->u.frac.den)));
        break;
    case BC_OP2_SUB:
        v = cfrac_new(ctx, cval_sub(ctx, cval_mul(ctx, cval_dup(v1->u.frac.num),
                                        cval_dup(v2->u.frac.den)), 
                                    cval_mul(ctx, cval_dup(v2->u.frac.num),
                                             cval_dup(v1->u.frac.den))),
                      cval_mul(ctx, cval_dup(v1->u.frac.den),
                               cval_dup(v2->u.frac.den)));
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        v = cfrac_new(ctx, cval_mul(ctx, cval_dup(v1->u.frac.num),
                                    cval_dup(v2->u.frac.num)), 
                      cval_mul(ctx, cval_dup(v1->u.frac.den),
                               cval_dup(v2->u.frac.den)));
        break;
    case BC_OP2_DIV:
    case BC_OP2_FRAC_DIV:
        v = cfrac_new(ctx, cval_mul(ctx, cval_dup(v1->u.frac.num),
                                    cval_dup(v2->u.frac.den)), 
                      cval_mul(ctx, cval_dup(v1->u.frac.den),
                               cval_dup(v2->u.frac.num)));
        break;
    case BC_OP2_MOD:
        {
            BCValue q;
            /* XXX: not euclidian */
            q = cval_floor(ctx, cval_div(ctx, cval_dup(v1), cval_dup(v2)));
            v = cval_sub(ctx, cval_dup(v1),
                         cval_mul(ctx, q, cval_dup(v2)));
        }
        break;
    case BC_OP2_CMP_EQ:
    case BC_OP2_CMP_LT:
    case BC_OP2_CMP_LE:
        v = cint_op2(ctx, cval_mul(ctx, cval_dup(v1->u.frac.num),
                                   cval_dup(v2->u.frac.den)), 
                      cval_mul(ctx, cval_dup(v1->u.frac.den),
                               cval_dup(v2->u.frac.num)), op);
        break;
    default:
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue cval_frac_div(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_FRAC_DIV);
}

static int get_op1_rnd_mode(BCOP1Enum op)
{
    switch(op) {
    case BC_OP1_TRUNC:
        return BF_RNDZ;
    case BC_OP1_FLOOR:
        return BF_RNDD;
    case BC_OP1_CEIL:
        return BF_RNDU;
    case BC_OP1_ROUND:
        return BF_RNDNA;
    default:
        abort();
    }
}

static BCValue cfrac_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    
    switch(op) {
    case BC_OP1_NEG:
        v = cfrac_new2(ctx, cval_neg(ctx, cval_dup(v1->u.frac.num)),
                       cval_dup(v1->u.frac.den));
        break;
    case BC_OP1_ABS:
        v = cfrac_new2(ctx, cval_abs(ctx, cval_dup(v1->u.frac.num)),
                       cval_dup(v1->u.frac.den));
        break;
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
        /* return an integer */
        {
            bf_t r_s, *r = &r_s;
            BCValue num = v1->u.frac.num;
            BCValue den = v1->u.frac.den;

            v = cint_new(ctx);
            bf_init(&ctx->bf_ctx, r);
            bf_divrem(&v->u.cint, r, &num->u.cint, &den->u.cint, BF_PREC_INF, BF_RNDZ, get_op1_rnd_mode(op));
            bf_delete(r);
        }
        break;
    default:
        abort();
    }
    cval_free(ctx, v1);
    return v;
}

static void cfrac_toString(BCContext *ctx, DynBuf *d, BCValueConst v)
{
    cval_toString(ctx, d, v->u.frac.num);
    dbuf_putstr(d, "//");
    cval_toString(ctx, d, v->u.frac.den);
}

/*************************************************/
/* decimal */

static BCValue cdec_new(BCContext *ctx)
{
    BCValue v;
    v = cval_new(ctx, CTYPE_DECIMAL);
    bfdec_init(&ctx->bf_ctx, &v->u.cdec);
    return v;
}

/* 'v1' is not freed */
static void cdec_toString(BCContext *ctx, DynBuf *d, BCValueConst v1)
{
    char *str;
    str = bfdec_ftoa(NULL, &v1->u.cdec, BF_PREC_INF,
                     BF_RNDZ | BF_FTOA_FORMAT_FREE);
    dbuf_putstr(d, str);
    if (bfdec_is_finite(&v1->u.cdec) && !strchr(str, '.') && !strchr(str, 'e'))
        dbuf_putstr(d, ".0");
    bf_free(&ctx->bf_ctx, str);
}

static BCValue cdec_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op);

static BCValue to_dec1(BCContext *ctx, BCValue v1, BOOL allow_float)
{
    BCValue v;
    if (cval_type(v1) == CTYPE_BOOL) {
        v = cdec_new(ctx);
        bfdec_set_si(&v->u.cdec, cbool_to_int(v1));
    } else if (cval_type(v1) == CTYPE_INT) {
        v = cdec_new(ctx);
        bfdec_from_f(&v->u.cdec, &v1->u.cint, ctx->dec_prec, ctx->dec_flags);
    } else if (cval_type(v1) == CTYPE_DECIMAL) {
        return v1;
    } else if (cval_type(v1) == CTYPE_FRAC) {
        v = cdec_op2(ctx, cval_dup(v1->u.frac.num),
                       cval_dup(v1->u.frac.den),
                       BC_OP2_DIV);
    } else if (cval_type(v1) == CTYPE_FLOAT && allow_float) {
        v = cdec_new(ctx);
        bfdec_from_f(&v->u.cdec, &v1->u.cfloat, ctx->dec_prec, ctx->dec_flags);
    } else {
        v = cval_type_error(ctx, "cannot convert to decimal");
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue to_dec(BCContext *ctx, BCValue v1)
{
    return to_dec1(ctx, v1, FALSE);
}

static BCValue cdec_ctor(BCContext *ctx, BCValue v1)
{
    return to_dec1(ctx, v1, TRUE);
}

/* return an integer >= ceil(prec*log2(10)) */
#define DEC_TO_BIN_EXTRA_PREC 16

static limb_t dec_to_bin_prec(limb_t prec)
{
    return ((prec * 851 + 255) >> 8);
}

static BCValue cdec_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_dec(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_dec(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    v = cdec_new(ctx);
    switch(op) {
    case BC_OP2_ADD:
        bfdec_add(&v->u.cdec, &v1->u.cdec, &v2->u.cdec, ctx->dec_prec, ctx->dec_flags);
        break;
    case BC_OP2_SUB:
        bfdec_sub(&v->u.cdec, &v1->u.cdec, &v2->u.cdec, ctx->dec_prec, ctx->dec_flags);
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        bfdec_mul(&v->u.cdec, &v1->u.cdec, &v2->u.cdec, ctx->dec_prec, ctx->dec_flags);
        break;
    case BC_OP2_DIV:
        bfdec_div(&v->u.cdec, &v1->u.cdec, &v2->u.cdec, ctx->dec_prec, ctx->dec_flags);
        break;
    case BC_OP2_MOD:
        bfdec_rem(&v->u.cdec, &v1->u.cdec, &v2->u.cdec, ctx->dec_prec, ctx->dec_flags,
               BF_DIVREM_EUCLIDIAN);
        break;
    case BC_OP2_POW:
    case BC_OP2_ATAN2:
        /* XXX: no bfdec support for transcendtals, so we convert to
           binary float */
        /* XXX: not accurate */
        {
            bf_t t1, t2;
            limb_t prec1;
            prec1 = dec_to_bin_prec(ctx->dec_prec) + DEC_TO_BIN_EXTRA_PREC;
            bf_init(&ctx->bf_ctx, &t1);
            bf_init(&ctx->bf_ctx, &t2);
            bfdec_to_f(&t1, &v1->u.cdec, prec1, BF_RNDF);
            bfdec_to_f(&t2, &v2->u.cdec, prec1, BF_RNDF);
            switch(op) {
            case BC_OP2_POW:
                bf_pow(&t1, &t1, &t2, prec1, BF_RNDF);
                break;
            case BC_OP2_ATAN2:
                bf_atan2(&t1, &t1, &t2, prec1, BF_RNDF);
                break;
            default:
                abort();
            }
            bf_delete(&t2);
            bfdec_from_f(&v->u.cdec, &t1, ctx->dec_prec, ctx->dec_flags);
            bf_delete(&t1);
        }
        break;
    case BC_OP2_CMP_EQ:
        cval_free(ctx, v);
        v = cbool_new(ctx, bfdec_cmp_eq(&v1->u.cdec, &v2->u.cdec));
        break;
    case BC_OP2_CMP_LT:
        cval_free(ctx, v);
        v = cbool_new(ctx, bfdec_cmp_lt(&v1->u.cdec, &v2->u.cdec));
        break;
    case BC_OP2_CMP_LE:
        cval_free(ctx, v);
        v = cbool_new(ctx, bfdec_cmp_le(&v1->u.cdec, &v2->u.cdec));
        break;
    default:
        cval_free(ctx, v);
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue cdec_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    v = cdec_new(ctx);
    switch(op) {
    case BC_OP1_NEG:
        bfdec_set(&v->u.cdec, &v1->u.cdec);
        bfdec_neg(&v->u.cdec);
        break;
    case BC_OP1_ABS:
        bfdec_set(&v->u.cdec, &v1->u.cdec);
        v->u.cdec.sign = 0;
        break;
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
        bfdec_set(&v->u.cdec, &v1->u.cdec);
        bfdec_rint(&v->u.cdec, get_op1_rnd_mode(op));
        break;
    case BC_OP1_SQRT:
        bfdec_sqrt(&v->u.cdec, &v1->u.cdec, ctx->dec_prec, ctx->dec_flags);
        break;
    default:
        /* XXX: no bfdec support for transcendtals, so we convert to
           binary float */
        /* XXX: not accurate */
        {
            bf_t t0, t1;
            limb_t prec1;
            prec1 = dec_to_bin_prec(ctx->dec_prec) + DEC_TO_BIN_EXTRA_PREC;
            bf_init(&ctx->bf_ctx, &t0);
            bf_init(&ctx->bf_ctx, &t1);
            bfdec_to_f(&t0, &v1->u.cdec, prec1, BF_RNDF);
            switch(op) {
            case BC_OP1_EXP:
                bf_exp(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_LOG:
                bf_log(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_SIN:
                bf_sin(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_COS:
                bf_cos(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_TAN:
                bf_tan(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_ASIN:
                bf_asin(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_ACOS:
                bf_acos(&t1, &t0, prec1, BF_RNDF);
                break;
            case BC_OP1_ATAN:
                bf_atan(&t1, &t0, prec1, BF_RNDF);
                break;
            default:
                abort();
            }
            bf_delete(&t0);
            bfdec_from_f(&v->u.cdec, &t1, ctx->dec_prec, ctx->dec_flags);
            bf_delete(&t1);
        }
        break;
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue cdec_const(BCContext *ctx, BCConstEnum idx)
{
    BCConstDef *d = &ctx->const_tab[idx];
    BCValue v;
    if (d->prec == ctx->dec_prec) {
        return cval_dup(d->value);
    } else {
        bf_t t1;
        limb_t prec1;

        /* XXX: not accurate */
        prec1 = dec_to_bin_prec(ctx->dec_prec) + DEC_TO_BIN_EXTRA_PREC;
        bf_init(&ctx->bf_ctx, &t1);

        switch(idx) {
        case BC_CONST_PI:
            bf_const_pi(&t1, prec1, BF_RNDF);
            break;
        case BC_CONST_LOG2E:
            {
                bf_t t2;
                bf_const_log2(&t1, prec1, BF_RNDF);
                bf_init(&ctx->bf_ctx, &t2);
                bf_set_si(&t2, 1);
                bf_div(&t1, &t2, &t1, prec1, BF_RNDF);
                bf_delete(&t2);
            }
            break;
        case BC_CONST_LOG10E:
            {
                bf_t t2;
                bf_init(&ctx->bf_ctx, &t2);
                bf_set_si(&t2, 10);
                bf_log(&t1, &t2, prec1, BF_RNDF);
                bf_set_si(&t2, 1);
                bf_div(&t1, &t2, &t1, prec1, BF_RNDF);
                bf_delete(&t2);
            }
            break;
        case BC_CONST_LOG10:
            {
                bf_t t2;
                bf_init(&ctx->bf_ctx, &t2);
                bf_set_si(&t2, 10);
                bf_log(&t1, &t2, prec1, BF_RNDF);
                bf_delete(&t2);
            }
            break;
        default:
            abort();
        }
        v = cdec_new(ctx);
        bfdec_from_f(&v->u.cdec, &t1, ctx->dec_prec, ctx->dec_flags);
        bf_delete(&t1);
        
        cval_free(ctx, d->value);
        d->value = cval_dup(v);
        d->prec = ctx->dec_prec;
    }
    return v;
}

static BCValue cdec_pi(BCContext *ctx)
{
    return cdec_const(ctx, BC_CONST_PI);
}
 
/*************************************************/
/* float */

static BCValue cfloat_new(BCContext *ctx)
{
    BCValue v;
    v = cval_new(ctx, CTYPE_FLOAT);
    bf_init(&ctx->bf_ctx, &v->u.cfloat);
    return v;
}

/* 'v1' is not freed */
static void cfloat_toString(BCContext *ctx, DynBuf *d, BCValueConst v1, int radix)
{
    char *str;
    int flags1;
    flags1 = (ctx->float_flags &  (BF_FLAG_SUBNORMAL | (BF_EXP_BITS_MASK << BF_EXP_BITS_SHIFT)));
    str = bf_ftoa(NULL, &v1->u.cfloat, radix, ctx->float_prec,
                  flags1 | BF_RNDN | BF_FTOA_FORMAT_FREE_MIN | BF_FTOA_ADD_PREFIX | BF_FTOA_JS_QUIRKS);
    dbuf_putstr(d, str);
    if (bf_is_finite(&v1->u.cfloat)) {
        if (!strchr(str, '.') && !strchr(str, 'e'))
            dbuf_putstr(d, ".0");
        dbuf_putstr(d, "l");
    }
    bf_free(&ctx->bf_ctx, str);
}

static BCValue cfloat_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op);

static BCValue to_float1(BCContext *ctx, BCValue v1, BOOL allow_dec)
{
    BCValue v;
    if (cval_type(v1) == CTYPE_FLOAT) {
        return v1;
    } else if (cval_type(v1) == CTYPE_BOOL) {
        v = cdec_new(ctx);
        bf_set_si(&v->u.cfloat, cbool_to_int(v1));
    } else if (cval_type(v1) == CTYPE_INT) {
        v = cfloat_new(ctx);
        bf_set(&v->u.cfloat, &v1->u.cint);
        bf_round(&v->u.cfloat, ctx->float_prec, ctx->float_flags);
    } else if (cval_type(v1) == CTYPE_FRAC) {
        v = cfloat_op2(ctx, cval_dup(v1->u.frac.num),
                       cval_dup(v1->u.frac.den),
                       BC_OP2_DIV);
    } else if (cval_type(v1) == CTYPE_DECIMAL && allow_dec) {
        v = cfloat_new(ctx);
        bfdec_to_f(&v->u.cfloat, &v1->u.cdec,
                   ctx->float_prec, ctx->float_flags);
    } else {
        v = cval_type_error(ctx, "cannot convert to float");
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue to_float(BCContext *ctx, BCValue v1)
{
    return to_float1(ctx, v1, FALSE);
}

static BCValue cfloat_ctor(BCContext *ctx, BCValue v1)
{
    return to_float1(ctx, v1, TRUE);
}

static BCValue cfloat_from_double(BCContext *ctx, double d)
{
    BCValue v;
    v = cfloat_new(ctx);
    bf_set_float64(&v->u.cfloat, d);
    bf_round(&v->u.cfloat, ctx->float_prec, ctx->float_flags);
    return v;
}

static BCValue cfloat_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_float(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_float(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    v = cfloat_new(ctx);
    switch(op) {
    case BC_OP2_ADD:
        bf_add(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_SUB:
        bf_sub(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        bf_mul(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_DIV:
        bf_div(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_MOD:
        bf_rem(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags,
               BF_DIVREM_EUCLIDIAN);
        break;
    case BC_OP2_POW:
        bf_pow(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_ATAN2:
        bf_atan2(&v->u.cfloat, &v1->u.cfloat, &v2->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP2_CMP_EQ:
        cval_free(ctx, v);
        v = cbool_new(ctx, bf_cmp_eq(&v1->u.cfloat, &v2->u.cfloat));
        break;
    case BC_OP2_CMP_LT:
        cval_free(ctx, v);
        v = cbool_new(ctx, bf_cmp_lt(&v1->u.cfloat, &v2->u.cfloat));
        break;
    case BC_OP2_CMP_LE:
        cval_free(ctx, v);
        v = cbool_new(ctx, bf_cmp_le(&v1->u.cfloat, &v2->u.cfloat));
        break;
    default:
        cval_free(ctx, v);
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue cfloat_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    v = cfloat_new(ctx);
    switch(op) {
    case BC_OP1_NEG:
        bf_set(&v->u.cfloat, &v1->u.cfloat);
        bf_neg(&v->u.cfloat);
        break;
    case BC_OP1_ABS:
        bf_set(&v->u.cfloat, &v1->u.cfloat);
        v->u.cfloat.sign = 0;
        break;
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
        bf_set(&v->u.cfloat, &v1->u.cfloat);
        bf_rint(&v->u.cfloat, get_op1_rnd_mode(op));
        break;
    case BC_OP1_SQRT:
        bf_sqrt(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_EXP:
        bf_exp(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_LOG:
        bf_log(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_SIN:
        bf_sin(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_COS:
        bf_cos(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_TAN:
        bf_tan(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_ASIN:
        bf_asin(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_ACOS:
        bf_acos(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    case BC_OP1_ATAN:
        bf_atan(&v->u.cfloat, &v1->u.cfloat, ctx->float_prec, ctx->float_flags);
        break;
    default:
        abort();
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue cval_bestappr(BCContext *ctx, BCValue u, BCValue b)
{
    BCValue num, den, num1, den1, num0, den0, n;
    
    if (cval_type(u) != CTYPE_DECIMAL &&
        cval_type(u) != CTYPE_FLOAT) {
        u = to_dec(ctx, u);
        if (cval_is_error(u)) {
            cval_free(ctx, b);
            return BC_EXCEPTION;
        }
    }
    if (cval_type(b) != CTYPE_INT) {
        cval_free(ctx, u);
        cval_free(ctx, b);
        return BC_EXCEPTION;
    }

    num1 = cint_from_int(ctx, 1);
    num0 = cint_from_int(ctx, 0);
    den1 = cint_from_int(ctx, 0);
    den0 = cint_from_int(ctx, 1);
    for(;;) {
        n = to_cint(ctx, cval_floor(ctx, cval_dup(u)));
        num = cval_add(ctx, cval_mul(ctx, cval_dup(n), cval_dup(num1)), num0);
        den = cval_add(ctx, cval_mul(ctx, cval_dup(n), cval_dup(den1)), den0);
        if (cval_cmp_lt2(ctx, b, den))
            break;
        u = cval_inverse(ctx, cval_sub(ctx, u, n));
        num0 = num1;
        num1 = num;
        den0 = den1;
        den1 = den;
    }
    cval_free(ctx, n);
    cval_free(ctx, num);
    cval_free(ctx, den);
    cval_free(ctx, u);
    cval_free(ctx, b);
    return cfrac_new(ctx, num1, den1);
}

/*************************************************/
/* complex */

static BOOL is_real_number(const BCType *t)
{
    return (t->tag == CTYPE_INT ||
            t->tag == CTYPE_FRAC ||
            t->tag == CTYPE_DECIMAL ||
            t->tag == CTYPE_FLOAT);
}

static BOOL is_complex_frac(const BCType *t)
{
    return  (t->tag == CTYPE_FRAC ||
             (t->tag == CTYPE_COMPLEX &&
              (t->elem_type->tag == CTYPE_FRAC)));
}

static __maybe_unused BOOL is_complex_int(const BCType *t)
{
    return  (t->tag == CTYPE_INT ||
             (t->tag == CTYPE_COMPLEX &&
              (t->elem_type->tag == CTYPE_INT)));
}

static BOOL same_type(BCContext *ctx, const BCType *t1,
                      const BCType *t2)
{
    if (t1->tag != t2->tag)
        return FALSE;
    if (t1->elem_type)
        return same_type(ctx, t1->elem_type, t2->elem_type);
    else
        return TRUE;
}

static BCType *get_op2_type(BCContext *ctx, const BCType *t1,
                            const BCType *t2, BCOP2Enum op)
{
    const BCType *t0;

    if (op == BC_OP2_CMP_EQ ||
        op == BC_OP2_CMP_LT ||
        op == BC_OP2_CMP_LE) {
        return ctype_new(ctx, CTYPE_BOOL, NULL);
    }
    if (t1->tag < t2->tag) {
        t0 = t1;
        t1 = t2;
        t2 = t0;
    }

    /* special cases */
    if (op == BC_OP2_DIV && t1->tag == CTYPE_INT) {
        return ctype_dup(ctx->def_type[CTYPE_DECIMAL]);
    } else if (op == BC_OP2_FRAC_DIV && t1->tag == CTYPE_INT) {
        return ctype_dup(ctx->def_type[CTYPE_FRAC]);
    } else if (op == BC_OP2_POW && t1->tag == CTYPE_FRAC) {
        return ctype_dup(ctx->def_type[CTYPE_DECIMAL]);
    } else if (op == BC_OP2_ATAN2 && t1->tag <= CTYPE_FRAC) {
        return ctype_dup(ctx->def_type[CTYPE_DECIMAL]);
    }
    
    /* boolean are always promoted to int except in comparisons */
    /* XXX: fix tensors init to boolean */
    if (t1->tag == CTYPE_BOOL)
        return ctype_new(ctx, CTYPE_INT, NULL);
    
    if (same_type(ctx, t1, t2))
        return ctype_dup(t1);
    
    if (t1->tag == CTYPE_FLOAT && t2->tag == CTYPE_DECIMAL) {
        cval_type_error(ctx, "float and decimal are not compatible");
        return NULL;
    }
    if (t1->tag <= CTYPE_FLOAT) {
        return ctype_dup(t1);
    } else if (t1->tag == CTYPE_COMPLEX ||
               t1->tag == CTYPE_POLY ||
               t1->tag == CTYPE_SER ||
               t1->tag == CTYPE_RFRAC) {
        if (t2->tag == t1->tag)
            t2 = t2->elem_type;
        return ctype_new_free(ctx, t1->tag, get_op2_type(ctx, t1->elem_type, t2, op));
    } else {
        cval_type_error(ctx, "incompatible types");
        return NULL;
    }
}

static BCValue complex_new2(BCContext *ctx, BCValue re, BCValue im,
                            const BCType *el)
{
    BCValue r;
    BCType *t;
    t = ctype_new(ctx, CTYPE_COMPLEX, el);
    r = cval_new1(ctx, t);
    ctype_free(ctx, t);
    r->u.complex.re = cval_convert(ctx, re, el);
    r->u.complex.im = cval_convert(ctx, im, el);
    return r;
}

static BCValue complex_new(BCContext *ctx, BCValue re, BCValue im)
{
    BCValue r;
    BCType *el;
    if (!is_real_number(re->type) || !is_real_number(im->type))
        goto type_error;
    el = get_op2_type(ctx, re->type, im->type, BC_OP2_ADD);
    if (!el) {
    type_error:
        cval_free(ctx, re);
        cval_free(ctx, im);
        return cval_type_error(ctx, "incompatible types for complex data");
    }
    r = complex_new2(ctx, re, im, el);
    ctype_free(ctx, el);
    return r;
}

static BCValue complex_new_int (BCContext *ctx, int re, int im)
{
    return complex_new(ctx, cint_from_int(ctx, re), cint_from_int(ctx, im));
}

static BCValue to_complex(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) == CTYPE_COMPLEX)
        return v1;
    else
        return complex_new(ctx, v1, cint_from_int(ctx, 0));
}

/* insert a '+' at pos */
static void insert_plus(DynBuf *d, int pos)
{
    dbuf_putc(d, ' ');
    memmove(d->buf + pos + 1, d->buf + pos, d->size - pos - 1);
    d->buf[pos] = '+';
}

static void complex_toString(BCContext *ctx, DynBuf *d, BCValueConst v1)
{
    BCValue re = v1->u.complex.re;
    BCValue im = v1->u.complex.im;
    BOOL non_zero_re = FALSE;
    int pos;
    
    if (!cval_cmp_eq_int(ctx, re, 0)) {
        cval_toString(ctx, d, re);
        non_zero_re = TRUE;
    }
    pos = d->size;
    cval_toString(ctx, d, im);
    dbuf_putc(d, 'i');
    if (d->buf[pos] != '-' && non_zero_re) {
        insert_plus(d, pos);
    }
}

static BCValue complex_norm2(BCContext *ctx, BCValue v1)
{
    BCValue v;
    v1 = to_complex(ctx, v1);
    if (cval_is_error(v1))
        return v1;
    v = cval_add(ctx, cval_mul(ctx, cval_dup(v1->u.complex.re), cval_dup(v1->u.complex.re)),
                 cval_mul(ctx, cval_dup(v1->u.complex.im), cval_dup(v1->u.complex.im)));
    cval_free(ctx, v1);
    return v;
}

static BCValue complex_sqrt(BCContext *ctx, BCValue a)
{
    BCValue v, t, u, re, im;

    a = to_complex(ctx, a);
    if (cval_is_error(a))
        return a;
    t = cval_abs(ctx, cval_dup(a));
    u = cval_dup(a->u.complex.re);
    re = cval_sqrt(ctx, cval_div(ctx, cval_add(ctx, cval_dup(t), cval_dup(u)),
                                 cint_from_int(ctx, 2)));
    im = cval_sqrt(ctx, cval_div(ctx, cval_sub(ctx, t, u),
                                 cint_from_int(ctx, 2)));
    if (cval_cmp_lt_int(ctx, a->u.complex.im, 0))
        im = cval_neg(ctx, im);
    v = complex_new(ctx, re, im);
    cval_free(ctx, a);
    return v;
}

/* mutiply by 'I' */
static BCValue complex_muli(BCContext *ctx, BCValue v1)
{
    BCValue v;
    assert(cval_type(v1) == CTYPE_COMPLEX);
    v = complex_new(ctx, cval_neg(ctx, cval_dup(v1->u.complex.im)),
                    cval_dup(v1->u.complex.re));
    cval_free(ctx, v1);
    return v;
}

static BCValue complex_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    switch(op) {
    case BC_OP1_NEG:
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
        v = complex_new(ctx, cval_op1(ctx, cval_dup(v1->u.complex.re), op),
                        cval_op1(ctx, cval_dup(v1->u.complex.im), op));
        break;
    case BC_OP1_ABS:
        return cval_sqrt(ctx, complex_norm2(ctx, v1));
    case BC_OP1_CONJ:
        v = complex_new(ctx, cval_dup(v1->u.complex.re),
                        cval_neg(ctx, cval_dup(v1->u.complex.im)));
        break;
    case BC_OP1_RE:
        v = cval_dup(v1->u.complex.re);
        break;
    case BC_OP1_IM:
        v = cval_dup(v1->u.complex.im);
        break;
    case BC_OP1_SQRT:
        return complex_sqrt(ctx, v1);
    case BC_OP1_EXP:
        {
            BCValue arg, r;
            arg = cval_dup(v1->u.complex.im);
            r = cval_exp(ctx, cval_dup(v1->u.complex.re));
            v = complex_new(ctx, cval_mul(ctx, cval_cos(ctx, cval_dup(arg)), cval_dup(r)), 
                            cval_mul(ctx, cval_sin(ctx, cval_dup(arg)), cval_dup(r)));
            cval_free(ctx, r);
            cval_free(ctx, arg);
        }
        break;
    case BC_OP1_LOG:
        {
            BCValue arg, r;
            arg = cval_atan2(ctx, cval_dup(v1->u.complex.im), cval_dup(v1->u.complex.re));
            r = cval_abs(ctx, cval_dup(v1));
            v = complex_new(ctx, cval_log(ctx, r), arg);
        }
        break;
    case BC_OP1_SIN:
        {
            BCValue t;
            t = cval_exp(ctx, complex_muli(ctx, cval_dup(v1)));
            v = cval_div(ctx, cval_sub(ctx, cval_dup(t), cval_inverse(ctx, cval_dup(t))),
                         complex_new(ctx, cint_from_int(ctx, 0), cint_from_int(ctx, 2)));
            cval_free(ctx, t);
        }
        break;
    case BC_OP1_COS:
        {
            BCValue t;
            t = cval_exp(ctx, complex_muli(ctx, cval_dup(v1)));
            v = cval_div(ctx, cval_add(ctx, cval_dup(t), cval_inverse(ctx, cval_dup(t))),
                         cint_from_int(ctx, 2));
            cval_free(ctx, t);
        }
        break;
    case BC_OP1_TAN:
        {
            BCValue t, t1, t2;
            t = cval_exp(ctx, complex_muli(ctx, cval_dup(v1)));
            t1 = cval_inverse(ctx, cval_dup(t));
            t2 = cval_div(ctx, cval_sub(ctx, cval_dup(t), cval_dup(t1)),
                         cval_add(ctx, cval_dup(t), cval_dup(t1)));
            v = complex_new(ctx, cval_dup(t2->u.complex.im),
                            cval_neg(ctx, cval_dup(t2->u.complex.re)));
            cval_free(ctx, t);
            cval_free(ctx, t1);
            cval_free(ctx, t2);
        }
        break;
    default:
        cval_free(ctx, v1);
        return cval_type_error(ctx, "unsupported type");
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue complex_inverse(BCContext *ctx, BCValue v1)
{
    BCValue c, v;
    v1 = to_complex(ctx, v1);
    if (cval_is_error(v1))
        return v1;
    c = complex_norm2(ctx, cval_dup(v1));
    v = complex_new(ctx, cval_div(ctx, cval_dup(v1->u.complex.re), cval_dup(c)),
                    cval_neg(ctx, cval_div(ctx, cval_dup(v1->u.complex.im), cval_dup(c))));
    cval_free(ctx, c);
    cval_free(ctx, v1);
    return v;
}

static BCValue to_complex_frac(BCContext *ctx, BCValue v1)
{
    BCValue v;
    int tag = v1->type->elem_type->tag;
    if (tag == CTYPE_FRAC)
        return v1;
    if (tag != CTYPE_INT) {
        v = cval_type_error(ctx, "integer or fractional complex expected");
    } else {
        v = complex_new(ctx, to_cfrac(ctx, cval_dup(v1->u.complex.re)),
                        to_cfrac(ctx, cval_dup(v1->u.complex.im)));
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue complex_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_complex(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_complex(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    switch(op) {
    case BC_OP2_ADD:
        v = complex_new(ctx,
                        cval_add(ctx, cval_dup(v1->u.complex.re), cval_dup(v2->u.complex.re)),
                        cval_add(ctx, cval_dup(v1->u.complex.im), cval_dup(v2->u.complex.im)));
        break;
    case BC_OP2_SUB:
        v = complex_new(ctx,
                        cval_sub(ctx, cval_dup(v1->u.complex.re), cval_dup(v2->u.complex.re)),
                        cval_sub(ctx, cval_dup(v1->u.complex.im), cval_dup(v2->u.complex.im)));
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        v = complex_new(ctx,
                        cval_sub(ctx,
                                 cval_mul(ctx, cval_dup(v1->u.complex.re), cval_dup(v2->u.complex.re)),
                                 cval_mul(ctx, cval_dup(v1->u.complex.im), cval_dup(v2->u.complex.im))),
                        cval_add(ctx,
                                 cval_mul(ctx, cval_dup(v1->u.complex.re), cval_dup(v2->u.complex.im)),
                                 cval_mul(ctx, cval_dup(v1->u.complex.im), cval_dup(v2->u.complex.re))));
        break;
    case BC_OP2_DIV:
    case BC_OP2_FRAC_DIV:
        if (v2->type->elem_type->tag == CTYPE_INT &&
            (op == BC_OP2_FRAC_DIV || v1->type->elem_type->tag == CTYPE_FRAC)) {
            v2 = to_complex_frac(ctx, v2);
        }
        v = complex_op2(ctx, v1, complex_inverse(ctx, v2), BC_OP2_MUL);
        goto done;
    case BC_OP2_POW:
        v = cval_exp(ctx, cval_mul(ctx, cval_log(ctx, v1), v2));
        goto done;
    case BC_OP2_CMP_EQ:
        {
            int res;
            res = cval_cmp_eq2(ctx, v1->u.complex.re, v2->u.complex.re);
            res = res && cval_cmp_eq2(ctx, v1->u.complex.im, v2->u.complex.im);
            v = cbool_new(ctx, res);
        }
        break;
    case BC_OP2_CMP_LT:
    case BC_OP2_CMP_LE:
        v = cval_type_error(ctx, "complex numbers are not comparable");
        break;
    case BC_OP2_DIVREM:
        /* Gaussian integer Euclidian division */
        {
            BCValue q, r;
            if (v1->type->elem_type->tag != CTYPE_INT ||
                v2->type->elem_type->tag != CTYPE_INT) {
                v = cval_type_error(ctx, "both complex must have integer components for divrem");
                break;
            }
            q = cval_frac_div(ctx, cval_dup(v1), cval_dup(v2));
            if (cval_is_error(q)) {
                v = BC_EXCEPTION;
                break;
            }
            q = cval_round(ctx, q);
            r = cval_sub(ctx, cval_dup(v1), cval_mul(ctx, cval_dup(v2),
                                                     cval_dup(q)));
            v = carray_pair(ctx, q, r);
        }
        break;
    default:
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
 done:
    return v;
}

/*************************************************/
/* string */

static BCValue cstring_new(BCContext *ctx, const char *str, int len)
{
    BCValue v;
    if (len < 0)
        len = strlen(str);

    v = cval_new(ctx, CTYPE_STRING);
    v->u.string.len = len;
    v->u.string.str = malloc(len + 1);
    memcpy(v->u.string.str, str, len);
    v->u.string.str[len] = '\0';
    return v;
}

static void cstring_toString(BCContext *ctx, DynBuf *d, BCValueConst v)
{
    const BCString *str = &v->u.string;
    int i, c;

    dbuf_putc(d, '\"');
    for(i = 0; i < str->len; i++) {
        c = str->str[i];
        switch(c) {
        case '\t':
            c = 't';
            goto quote;
        case '\r':
            c = 'r';
            goto quote;
        case '\n':
            c = 'n';
            goto quote;
        case '\b':
            c = 'b';
            goto quote;
        case '\f':
            c = 'f';
            goto quote;
        case '\"':
        case '\\':
        quote:
            dbuf_putc(d, '\\');
            dbuf_putc(d, c);
            break;
        default:
            if (c < 32 || (c >= 0xd800 && c < 0xe000)) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                dbuf_putstr(d, buf);
            } else {
                dbuf_putc(d, c);
            }
            break;
        }
    }
    dbuf_putc(d, '\"');
}

static BCValue cstring_concat(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCString *st1 = &v1->u.string;
    BCString *st2 = &v2->u.string;
    BCString *st;
    BCValue v;
    int len;

    len = st1->len + st2->len;
    v = cval_new(ctx, CTYPE_STRING);
    st = &v->u.string;
    st->len = len;
    st->str = malloc(len + 1);
    memcpy(st->str, st1->str, st1->len);
    memcpy(st->str + st1->len, st2->str, st2->len);
    st->str[len] = '\0';
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static int cstring_len(BCContext *ctx, BCValueConst v1)
{
    const BCString *st = &v1->u.string;
    int c, i, len;

    len = 0;
    for(i = 0; i < st->len; i++) {
        c = st->str[i];
        if (c < 0x80 || c >= 0xc0)
            len++;
    }
    return len;
}

static size_t utf8_pos(const uint8_t *str, size_t char_pos)
{
    const uint8_t *p = str;
    size_t i;
    for(i = 0; i < char_pos; i++) {
        p++;
        while (*p >= 0x80 && *p < 0xc0)
            p++;
    }
    return p - str;
}
    
static BCValue cstring_slice(BCContext *ctx, BCValueConst v1,
                             size_t start, size_t end)
{
    const BCString *st = &v1->u.string;
    size_t start1, end1;
    if (start == end) {
        return cstring_new(ctx, NULL, 0);
    }
    start1 = utf8_pos(st->str, start);
    end1 = start1 + utf8_pos(st->str + start1, end - start);
    return cstring_new(ctx, (char *)(st->str + start1), end1 - start1);
}

static BCValue cstring_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue v, val;
    int i, len;

    if (n_args != 2) {
        val = cval_type_error(ctx, "strings have a single dimension");
        goto done;
    }
    v = args[0];
    len = cstring_len(ctx, v);
    if (cval_type(args[1]) == CTYPE_RANGE) {
        int start, stop;
        /* slice */
        start = args[1]->u.range.start;
        if (start == RANGE_DEFAULT)
            start = 0;
        stop = args[1]->u.range.stop;
        if (stop == RANGE_DEFAULT)
            stop = len;
        if (start < 0)
            start += len;
        if (stop < 0)
            stop += len;
        if (start < 0)
            start = 0;
        start = clamp_int(start, 0, len);
        stop = clamp_int(stop, 0, len);
        if (stop < start)
            stop = start;
        val = cstring_slice(ctx, v, start, stop);
    } else {
        int idx;
        if (cint_to_int(ctx, &idx, args[1])) {
            val = BC_EXCEPTION;
            goto done;
        }
        if (idx < 0)
            idx += len;
        if (idx < 0 || idx >= len) {
            val = cval_range_error(ctx, "array index out of bounds");
            goto done;
        }
        val = cstring_slice(ctx, v, idx, idx + 1);
    }
 done:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return val;
}

static BCValue cstring_chr(BCContext *ctx, BCValue v1)
{
    int c, len, res;
    uint8_t buf[UTF8_CHAR_LEN_MAX];
    
    res = cint_to_int(ctx, &c, v1);
    cval_free(ctx, v1);
    if (res)
        return BC_EXCEPTION;
    if (c < 0 || c > 0x10ffff)
        return cval_range_error(ctx, "invalid range for unicode character");
    len = unicode_to_utf8(buf, c);
    return cstring_new(ctx, (char *)buf, len);
}

static BCValue cstring_ord(BCContext *ctx, BCValue v1)
{
    BCString *st;
    int c;
    const uint8_t *p;

    if (cval_type(v1) != CTYPE_STRING) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "string expected");
    }
    st = &v1->u.string;
    if (st->len == 0)
        goto fail;
    c = unicode_from_utf8(st->str, st->len, &p);
    if ((p - st->str) != st->len) {
    fail:
        cval_free(ctx, v1);
        return cval_range_error(ctx, "expecting a string of one character");
    }
    cval_free(ctx, v1);
    return cint_from_int(ctx, c);
}

/*************************************************/
/* array */

static BCValue carray_new(BCContext *ctx, int allocated_len)
{
    BCValue v;
    BCArray *arr;

    v = cval_new(ctx, CTYPE_ARRAY);
    arr = &v->u.array;
    arr->size = allocated_len;
    arr->len = 0;
    arr->tab = malloc(sizeof(arr->tab[0]) * allocated_len);
    return v;
}

static BCValue carray_ctor(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue v;
    BCArray *arr;

    int i;
    v = carray_new(ctx, n_args);
    arr = &v->u.array;
    arr->len = n_args;
    for(i = 0; i < n_args; i++) {
        arr->tab[i] = args[i];
    }
    return v;
}

static void set_value(BCContext *ctx, BCValue *pval, BCValue v)
{
    BCValue v1;
    v1 = *pval;
    *pval = v;
    /* do the value after in case it modifies 'pval' */
    cval_free(ctx, v1);
}

static BCValue carray_getsetitem(BCContext *ctx, int n_args, BCValue *args,
                                  BOOL is_set)
{
    BCValue v, val;
    BCArray *arr;
    int i;

    if (n_args != 2 + is_set) {
        val = cval_type_error(ctx, "arrays have a single dimension");
        goto done;
    }
    v = args[0];
    arr = &v->u.array;
    if (cval_type(args[1]) == CTYPE_RANGE) {
        int start, stop, len;
        /* slice */
        start = args[1]->u.range.start;
        if (start == RANGE_DEFAULT)
            start = 0;
        stop = args[1]->u.range.stop;
        if (stop == RANGE_DEFAULT)
            stop = arr->len;
        if (start < 0)
            start += arr->len;
        if (stop < 0)
            stop += arr->len;
        if (start < 0)
            start = 0;
        start = clamp_int(start, 0, arr->len);
        stop = clamp_int(stop, 0, arr->len);
        if (stop < start)
            stop = start;
        len = stop - start;
        
        if (is_set) {
            BCValue v1;
            v1 = args[2];
            if (cval_type(v1) != CTYPE_ARRAY) {
                val = cval_type_error(ctx, "right hand side of array slice assignment must be an array");
                goto done;
            }
            /* XXX: no insertion nor deletion */
            if (v1->u.array.len != len) {
                val = cval_type_error(ctx, "invalid length of right hande side array");
                goto done;
            }
            for(i = 0; i < len; i++) {
                set_value(ctx, &arr->tab[start + i], cval_dup(v1->u.array.tab[i]));
            }
            val = cnull_new(ctx);
        } else {
            val = carray_new(ctx, len);
            val->u.array.len = len;
            for(i = 0; i < len; i++) {
                val->u.array.tab[i] = cval_dup(arr->tab[start + i]);
            }
        }
    } else {
        int idx;
        if (cint_to_int(ctx, &idx, args[1])) {
            val = BC_EXCEPTION;
            goto done;
        }
        if (idx < 0)
            idx += arr->len;
        if (idx < 0 || idx >= arr->len) {
            val = cval_range_error(ctx, "array index out of bounds");
            goto done;
        }
        if (is_set) {
            set_value(ctx, &arr->tab[idx], cval_dup(args[2]));
            val = cnull_new(ctx);
        } else {
            val = cval_dup(arr->tab[idx]);
        }
    }
 done:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return val;
}

static BCValue carray_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    return carray_getsetitem(ctx, n_args, args, FALSE);
}

static BCValue carray_setitem(BCContext *ctx, int n_args, BCValue *args)
{
    return carray_getsetitem(ctx, n_args, args, TRUE);
}

static BCValue carray_push(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue v = args[0];
    BCArray *arr;
    int n, new_len, new_size, i;

    arr = &v->u.array;
    n = n_args - 1;
    new_len = arr->len + n;
    if (new_len > arr->size) {
        new_size = max_int(new_len, arr->size * 3 / 2);
        arr->tab = realloc(arr->tab, sizeof(arr->tab[0]) * new_size);
        arr->size = new_size;
    }
    for(i = 0; i < n; i++) {
        arr->tab[arr->len + i] = args[1 + i];
    }
    arr->len = new_len;
    cval_free(ctx, v);
    return cnull_new(ctx);
}

static int carray_push1(BCContext *ctx, BCValueConst tab, BCValue a)
{
    BCValue args[2], r;

    args[0] = cval_dup(tab);
    args[1] = a;
    r = carray_push(ctx, 2, args);
    if (cval_is_error(r))
        return -1;
    cval_free(ctx, r);
    return 0;
}

static BCValue cval_len(BCContext *ctx, BCValue v1)
{
    BCValue v;
    if (cval_type(v1) == CTYPE_ARRAY) {
        v = cint_from_int(ctx, v1->u.array.len);
    } else if (cval_type(v1) == CTYPE_STRING) {
        v = cint_from_int(ctx, cstring_len(ctx, v1));
    } else {
        v = cval_type_error(ctx, "unsupported type for len");
    }
    cval_free(ctx, v1);
    return v;
}

static void carray_toString(BCContext *ctx, DynBuf *d, BCValueConst v)
{
    const BCArray *arr = &v->u.array;
    int i;

    if (ctx->js_mode)
        dbuf_putc(d, '[');
    else
        dbuf_putstr(d, "Array(");
    for(i = 0; i < arr->len; i++) {
        if (i != 0)
            dbuf_putstr(d, ", ");
        cval_toString(ctx, d, arr->tab[i]);
    }
    if (ctx->js_mode)
        dbuf_putc(d, ']');
    else
        dbuf_putc(d, ')');
}

static BCValue carray_pair(BCContext *ctx, BCValue q, BCValue r)
{
    BCValue v;
    v = carray_new(ctx, 2);
    v->u.array.len = 2;
    v->u.array.tab[0] = q;
    v->u.array.tab[1] = r;
    return v;
}

/*************************************************/
/* tensor */

static BCValue tensor_new(BCContext *ctx, const BCType *elem_type,
                          int n_dims, const int *dims)
{
    BCValue val;
    BCTensor *tr;
    BCType *t;
    int i;
    
    if (n_dims > MAX_DIMS) {
        return cval_type_error(ctx, "too many axis");
    }
    switch(elem_type->tag) {
        /* XXX: accept bool ? */
    case CTYPE_INT:
    case CTYPE_FRAC:
    case CTYPE_DECIMAL:
    case CTYPE_FLOAT:
    case CTYPE_COMPLEX:
    case CTYPE_POLY:
    case CTYPE_RFRAC:
    case CTYPE_SER:
        break;
    default:
        return cval_type_error(ctx, "only numeric types are allowed in tensors");
    }
    t = ctype_new(ctx, CTYPE_TENSOR, elem_type);
    val = cval_new1(ctx, t);
    ctype_free(ctx, t);
    tr = &val->u.tensor;
    tr->n_dims = n_dims;
    tr->size = 1;
    for(i = 0; i < n_dims; i++) {
        tr->dims[i] = dims[i];
        tr->size *= dims[i];
    }
    tr->tab = malloc(sizeof(tr->tab[0]) * tr->size);
    for(i = 0; i < tr->size; i++) {
        tr->tab[i] = cval_convert(ctx, cint_from_int(ctx, 0), elem_type);
    }
    return val;
}

static BCValue tensor_new_2d(BCContext *ctx, const BCType *elem_type,
                             int h, int w)
{
    int dims[2];
    dims[1] = h;
    dims[0] = w;
    return tensor_new(ctx, elem_type, 2, dims);
}

/* return FALSE if end of array and a_pos[0] is set to -1. */
static BOOL tensor_pos_incr(int *a_pos, int n_dims, const int *dims,
                           int first_axis)
{
    int i;
    for(i = first_axis; i < n_dims; i++) {
        if (++a_pos[i] != dims[i])
            return TRUE;
        a_pos[i] = 0;
    }
    a_pos[0] = -1;
    return FALSE;
}

static BCValue tensor_from_array(BCContext *ctx, BCValue v)
{
    int n_dims, i, j, n;
    BCValue v1, res;
    BCType *target_type;
    BCArray *arr;
    int a_pos[MAX_DIMS], dims[MAX_DIMS], pos;
    BCValue a_val[MAX_DIMS];
    BCTensor *tr;
    
    n_dims = 0;
    v1 = v;
    while (cval_type(v1) == CTYPE_ARRAY) {
        arr = &v1->u.array;
        if (arr->len < 1) {
            cval_free(ctx, v);
            return cval_type_error(ctx, "unexpected empty array");
        }
        v1 = arr->tab[0];
        n_dims++;
    }
    if (n_dims == 0) {
        cval_free(ctx, v);
        return cval_type_error(ctx, "array expected");
    }
    if (n_dims > MAX_DIMS) {
        cval_free(ctx, v);
        return cval_type_error(ctx, "too many axis");
    }
    
    i = n_dims;
    v1 = v;
    while (cval_type(v1) == CTYPE_ARRAY) {
        arr = &v1->u.array;
        i--;
        dims[i] = arr->len;
        a_val[i] = v1; /* a_val[i] has dimension dims[i] */
        v1 = arr->tab[0];
    }
    /* use a default integer type, then change it */
    res = tensor_new(ctx, ctx->def_type[CTYPE_INT], n_dims, dims);
    tr = &res->u.tensor;
    
    for(i = 0; i < n_dims; i++)
        a_pos[i] = 0;
    pos = 0;
    n = 0;
    target_type = NULL;
    for(;;) {
        for(j = n; j >= 0;) {
            v1 = a_val[j];
            if (cval_type(v1) != CTYPE_ARRAY) {
                cval_type_error(ctx, "array expected");
                goto fail;
            }
            arr = &v1->u.array;
            if (arr->len != tr->dims[j]) {
                cval_type_error(ctx, "unexpected array length (%d, expected %d)",
                                arr->len, dims[j]);
                goto fail;
            }
            v1 = arr->tab[a_pos[j]];
            if (j == 0)
                break;
            j--;
            a_val[j] = v1;
        }
        //        cval_dump(ctx, "v1", v1);
        if (!target_type) {
            target_type = ctype_dup(v1->type);
        } else {
            BCType *t0;
            t0 = get_op2_type(ctx, target_type, v1->type, BC_OP2_ADD);
            if (!t0)
                goto fail;
            ctype_free(ctx, target_type);
            target_type = t0;
        }
        set_value(ctx, &tr->tab[pos], cval_dup(v1));
        
        if (!tensor_pos_incr(a_pos, tr->n_dims, tr->dims, 0))
            break;

        n = 0;
        while (a_pos[n] == 0)
            n++;
        pos++;
    }

    /* set the correct element type */
    ctype_free(ctx, res->type->elem_type);
    res->type->elem_type = ctype_dup(target_type);
    
    for(i = 0; i < tr->size; i++) {
        v1 = cval_convert(ctx, cval_dup(tr->tab[i]), target_type);
        if (cval_is_error(v1))
            goto fail;
        set_value(ctx, &tr->tab[i], v1);
    }
    
    ctype_free(ctx, target_type);
    cval_free(ctx, v);
    return res;
 fail:
    ctype_free(ctx, target_type);
    cval_free(ctx, v);
    cval_free(ctx, res);
    return BC_EXCEPTION;
}

/* takes an array is parameter or dimensions */
static BCValue tensor_ctor(BCContext *ctx, BCValue arg)
{
    BCValue val;
    
    if (cval_type(arg) == CTYPE_ARRAY) {
        return tensor_from_array(ctx, arg);
    } else if (cval_type(arg) == CTYPE_TENSOR) {
        /* already a tensor: return it */
        return arg;
    } else {
        val = tensor_new(ctx, arg->type, 0, NULL);
        if (!cval_is_error(val)) {
            set_value(ctx, &val->u.tensor.tab[0], cval_dup(arg));
        }
        cval_free(ctx, arg);
        return val;
    }
}

static BCValue tensor_zeros(BCContext *ctx, int n_args, BCValue *args)
{
    int i;
    BCValue val;
    int a, dims[MAX_DIMS];
    
    if (n_args > MAX_DIMS) {
        val = cval_type_error(ctx, "too many axis in tensor");
        goto done;
    }
    for(i = 0; i < n_args; i++) {
        if (cint_to_int(ctx, &a, args[i])) {
            val = BC_EXCEPTION;
            goto done;
        }
        if (a < 1) {
            val = cval_type_error(ctx, "dimension must be a positive integer");
            goto done;
        }
        dims[n_args - 1 - i] = a;
    }
    val = tensor_new(ctx, ctx->def_type[CTYPE_INT], n_args, dims);

 done:
    for(i = 0; i < n_args;i++)
        cval_free(ctx, args[i]);
    return val;
}

static void tensor_toString(BCContext *ctx, DynBuf *d, BCValueConst v)
{
    const BCTensor *tr = &v->u.tensor;
    int i, j, line_size, n;
    int a_pos[MAX_DIMS];

    if (tr->n_dims == 0 || ctx->js_mode)
        dbuf_printf(d, "Tensor(");
    if (tr->n_dims == 0) {
        cval_toString(ctx, d, tr->tab[0]);
    } else {
        for(j = 0; j < tr->n_dims; j++)
            a_pos[j] = 0;
        line_size = tr->dims[0];

        n = tr->n_dims;
        
        for(i = 0; i < tr->size; i += line_size) {
            if (ctx->tensor_output_lf) {
                if (i != 0) {
                    dbuf_putstr(d, "       ");
                }
                for(j = 0; j < tr->n_dims - n; j++)
                    dbuf_putc(d, ' ');
            }
            for(j = tr->n_dims - n; j < tr->n_dims; j++)
                dbuf_putc(d, '[');

            for(j = 0; j < line_size; j++) {
                if (j != 0)
                    dbuf_putstr(d, ", ");
                cval_toString(ctx, d, tr->tab[i + j]);
            }
            
            if (!tensor_pos_incr(a_pos, tr->n_dims, tr->dims, 1))
                break;
            
            n = 0;
            while (a_pos[n] == 0)
                n++;
            
            for(j = 0; j < n; j++)
                dbuf_putc(d, ']');
            dbuf_putc(d, ',');
            if (ctx->tensor_output_lf)
                dbuf_putc(d, '\n');
            else
                dbuf_putc(d, ' ');
        }
        for(j = 0; j < tr->n_dims; j++)
            dbuf_putc(d, ']');
    }
    if (tr->n_dims == 0 || ctx->js_mode)
        dbuf_putc(d, ')');
}

static BCValue tensor_convert(BCContext *ctx, BCValue v1,
                              const BCType *elem_type)
{
    BCTensor *tr, *tr1;
    BCValue v, e;
    int i;
    tr1 = &v1->u.tensor;
    v = tensor_new(ctx, elem_type, tr1->n_dims, tr1->dims);
    tr = &v->u.tensor;
    for(i = 0; i < tr->size; i++) {
        e = cval_convert(ctx, cval_dup(tr1->tab[i]), elem_type);
        if (cval_is_error(e)) {
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &tr->tab[i], e);
    }
 done:
    cval_free(ctx, v1);
    return v;
}

static BCType *get_op1_type(BCContext *ctx, const BCType *t1,
                            BCOP1Enum op)
{
    switch(op) {
    case BC_OP1_NEG:
    case BC_OP1_CONJ:
        return ctype_dup(t1);
    case BC_OP1_TRUNC:
    case BC_OP1_FLOOR:
    case BC_OP1_CEIL:
    case BC_OP1_ROUND:
        {
            if (t1->tag == CTYPE_FRAC) {
                return ctype_dup(ctx->def_type[CTYPE_INT]);
            } else if (t1->tag == CTYPE_COMPLEX &&
                       t1->elem_type->tag == CTYPE_FRAC) {
                return ctype_new(ctx, CTYPE_COMPLEX,
                                 ctx->def_type[CTYPE_INT]);
            } else {
                return ctype_dup(t1);
            }
        }
        break;
    case BC_OP1_ABS:
    case BC_OP1_RE:
    case BC_OP1_IM:
        if (t1->tag == CTYPE_COMPLEX) {
            return ctype_dup(t1->elem_type);
        } else {
            return ctype_dup(t1);
        }
        break;
    case BC_OP1_SQRT:
    case BC_OP1_EXP:
    case BC_OP1_LOG:
    case BC_OP1_SIN:
    case BC_OP1_COS:
    case BC_OP1_TAN:
    case BC_OP1_ASIN:
    case BC_OP1_ACOS:
    case BC_OP1_ATAN:
        if (t1->tag == CTYPE_BOOL ||
            t1->tag == CTYPE_INT ||
            t1->tag == CTYPE_FRAC) {
            return ctype_dup(ctx->def_type[CTYPE_DECIMAL]);
        } else if (t1->tag == CTYPE_COMPLEX) {
            const BCType *el;
            el = t1->elem_type;
            if (el->tag == CTYPE_INT ||
                el->tag == CTYPE_FRAC) {
                return ctype_new(ctx, CTYPE_COMPLEX,
                                 ctx->def_type[CTYPE_DECIMAL]);
            } else {
                return ctype_dup(t1);
            }
        } else {
            return ctype_dup(t1);
        }
        break;
    default:
        abort();
    }
}

static BCType *get_inverse2_type(BCContext *ctx, const BCType *t1, BOOL is_frac)
{
    if (t1->tag == CTYPE_INT) {
        if (is_frac)
            return ctype_dup(ctx->def_type[CTYPE_FRAC]);
        else
            return ctype_dup(ctx->def_type[CTYPE_DECIMAL]);
    } else if ((t1->tag == CTYPE_COMPLEX || t1->tag == CTYPE_SER) &&
               t1->elem_type->tag == CTYPE_INT) {
        return ctype_new_free(ctx, t1->tag,
                              get_inverse2_type(ctx, t1->elem_type, is_frac));
    } else {
        return ctype_dup(t1);
    }
}

static BCType *get_inverse_type(BCContext *ctx, const BCType *t1)
{
    return get_inverse2_type(ctx, t1, FALSE);
}

static BCValue tensor_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v, e1;
    BCTensor *tr, *tr1;
    BCType *elem_type;
    int i;
    
    tr1 = &v1->u.tensor;
    elem_type = get_op1_type(ctx, v1->type->elem_type, op);
    v = tensor_new(ctx, elem_type, tr1->n_dims, tr1->dims);
    ctype_free(ctx, elem_type);
    tr = &v->u.tensor;

    for(i = 0; i < tr->size; i++) {
        e1 = cval_op1(ctx, cval_dup(tr1->tab[i]), op);
        if (cval_is_error(e1)) {
            cval_free(ctx, v);
            v = e1;
            break;
        }
        set_value(ctx, &tr->tab[i], e1);
    }
    cval_free(ctx, v1);
    return v;
}

/* change the dimensions of 'v1' to 'dims' by broadcasting the elements */
static BCValue tensor_broadcast(BCContext *ctx, BCValue v1, int n_dims,
                                const int *dims)
{
    BCTensor *tr1, *tr;
    int strides[MAX_DIMS], a_pos[MAX_DIMS], i, j, n, pos;
    BCValue v;
    
    tr1 = &v1->u.tensor;
    /* check the most common case */
    if (tr1->n_dims == n_dims) {
        for(i = 0; i < n_dims; i++) {
            if (dims[i] != tr1->dims[i])
                goto do_broadcast;
        }
        return v1;
    }
 do_broadcast:
    n = 1;
    for(i = 0; i < n_dims; i++) {
        if (i >= tr1->n_dims || tr1->dims[i] == 1) {
            strides[i] = 0;
        } else {
            assert(dims[i] == tr1->dims[i]);
            strides[i] = n;
            n *= tr1->dims[i];
        }
    }
    assert(n == tr1->size);

    v = tensor_new(ctx, v1->type->elem_type, n_dims, dims);
    tr = &v->u.tensor;
    
    for(i = 0; i < n_dims; i++)
        a_pos[i] = 0;
    for(i = 0; i < tr->size; i++) {
        pos = 0;
        for(j = 0; j < n_dims; j++)
            pos += strides[j] * a_pos[j];
        set_value(ctx, &tr->tab[i], cval_dup(tr1->tab[pos]));
        tensor_pos_incr(a_pos, n_dims, dims, 0);
    }
    cval_free(ctx, v1);
    
    return v;
}

static BCValue to_tensor(BCContext *ctx, BCValue v1)
{
    BCValue v;
    if (cval_type(v1) == CTYPE_TENSOR)
        return v1;
    v = tensor_new(ctx, v1->type, 0, NULL);
    if (!cval_is_error(v)) {
        set_value(ctx, &v->u.tensor.tab[0], cval_dup(v1));
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue tensor_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    BCType *t = NULL;
    BCTensor *tr1, *tr2, *tr;
    int dims[MAX_DIMS], n_dims, i;


    v1 = to_tensor(ctx, v1);
    if (cval_is_error(v1)) {
        v = BC_EXCEPTION;
        goto done;
    }
    v2 = to_tensor(ctx, v2);
    if (cval_is_error(v2)) {
        v = BC_EXCEPTION;
        goto done;
    }

    /* specific cases */
    if (op == BC_OP2_MUL) {
        return matrix_mul(ctx, v1, v2);
    } else if (op == BC_OP2_DOT_MUL) {
        op = BC_OP2_MUL;
    }

    t = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type, op);
    if (!t) {
        v = BC_EXCEPTION;
        goto done;
    }

    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;

    /* compute the dimensions of the result */
    n_dims = max_int(tr1->n_dims, tr2->n_dims);
    for(i = 0; i < n_dims; i++) {
        int d1, d2, d;
        if (i >= tr1->n_dims)
            d1 = 1;
        else
            d1 = tr1->dims[i];
        if (i >= tr2->n_dims)
            d2 = 1;
        else
            d2 = tr2->dims[i];
        if (d1 == d2 || d2 == 1) {
            d = d1;
        } else if (d1 == 1) {
            d = d2;
        } else {
            cval_type_error(ctx, "incompatible tensor dimensions");
            v = BC_EXCEPTION;
            goto done;
        }
        dims[i] = d;
    }
    
    v1 = tensor_broadcast(ctx, v1, n_dims, dims);
    if (cval_is_error(v1))
        goto done;
    v2 = tensor_broadcast(ctx, v2, n_dims, dims);
    if (cval_is_error(v2))
        goto done;

    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;
    v = tensor_new(ctx, t, n_dims, dims);
    ctype_free(ctx, t);
    t = NULL;
    tr = &v->u.tensor;
    
    for(i = 0; i < tr->size; i++) {
        BCValue e1;
        e1 = cval_op2(ctx, cval_dup(tr1->tab[i]), cval_dup(tr2->tab[i]), op);
        if (cval_is_error(e1)) {
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &tr->tab[i], e1);
    }
    
 done:
    if (t)
        ctype_free(ctx, t);
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue tensor_shape(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCTensor *tr1;
    BCArray *arr;
    int i;
    
    if (cval_type(v1) != CTYPE_TENSOR) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "tensor expected");
    }
    tr1 = &v1->u.tensor;
    v = carray_new(ctx, tr1->n_dims);
    arr = &v->u.array;
    arr->len = tr1->n_dims;
    for(i = 0; i < tr1->n_dims; i++) {
        arr->tab[i] = cint_from_int(ctx, tr1->dims[tr1->n_dims - i - 1]);
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue tensor_getsetitem(BCContext *ctx, int n_args, BCValue *args,
                                 BOOL is_set)
{
    BCValue v, el, v1;
    BCTensor *tr1, *tr;
    int strides[MAX_DIMS], offsets[MAX_DIMS], dims[MAX_DIMS], a_pos[MAX_DIMS];
    int rdims[MAX_DIMS], n_rdims;
    int n_axis, i, j, n, n_dims, pos;

    v1 = args[0];
    tr1 = &v1->u.tensor;
    n_dims = tr1->n_dims;
    n_axis = n_args - 1 - is_set;
    if (n_axis > tr1->n_dims) {
        v = cval_type_error(ctx, "too many axis");
        goto done;
    }

    n_rdims = 0;
    /* compute the dimensions and offsets of the slice */
    for(i = 0; i < n_dims - n_axis; i++) {
        offsets[i] = 0;
        dims[i] = tr1->dims[i];
        rdims[n_rdims++] = dims[i];
    }

    for(i = n_dims - n_axis; i < n_dims; i++) {
        el = args[1 + n_axis - 1 - (i - (n_dims - n_axis))];
        if (cval_type(el) == CTYPE_RANGE) {
            int start, stop;
            start = el->u.range.start;
            stop = el->u.range.stop;
            if (start == RANGE_DEFAULT)
                start = 0;
            if (stop == RANGE_DEFAULT)
                stop = tr1->dims[i];
            if (start < 0)
                start += tr1->dims[i];
            if (stop < 0)
                stop += tr1->dims[i];
            if (stop <= start || start < 0 || stop > tr1->dims[i]) {
                v = cval_range_error(ctx, "tensor slice out of bounds");
                goto done;
            }
            offsets[i] = start;
            dims[i] = stop - start;
            rdims[n_rdims++] = dims[i];
        } else {
            int idx;
            if (cint_to_int(ctx, &idx, el)) {
                v = BC_EXCEPTION;
                goto done;
            }
            if (idx < 0)
                idx += dims[i];
            if (idx < 0 || idx >= tr1->dims[i]) {
                v = cval_range_error(ctx, "tensor index out of bounds");
                goto done;
            }
            offsets[i] = idx;
            dims[i] = 1;
        }
    }

    n = 1;
    for(i = 0; i < n_dims; i++) {
        strides[i] = n;
        n *= tr1->dims[i];
    }

    if (is_set) {
        BCValue e1, e2;
        const BCType *elem_type = v1->type->elem_type;
        e1 = args[n_args - 1];
        if (n_rdims == 0) {
            e1 = cval_convert(ctx, cval_dup(e1), elem_type);
            if (cval_is_error(e1)) {
                v = BC_EXCEPTION;
                goto done;
            }
            pos = 0;
            for(j = 0; j < n_dims; j++)
                pos += strides[j] * offsets[j];
            set_value(ctx, &tr1->tab[pos], e1);
        } else {
            if (cval_type(e1) != CTYPE_TENSOR) {
                v = cval_type_error(ctx, "tensor expected");
                goto done;
            }
            tr = &e1->u.tensor;
            if (tr->n_dims != n_rdims) {
                v = cval_type_error(ctx, "invalid number of dimensions");
                goto done;
            }
            for(i = 0; i < n_rdims; i++) {
                if (tr->dims[i] != rdims[i]) {
                    v = cval_type_error(ctx, "incompatible dimensions in slice assignment");
                    goto done;
                }
            }
            /* set the slice */
            for(i = 0; i < n_dims; i++)
                a_pos[i] = 0;
            for(i = 0; i < tr->size; i++) {
                pos = 0;
                for(j = 0; j < n_dims; j++)
                    pos += strides[j] * (a_pos[j] + offsets[j]);
                e2 = cval_convert(ctx, cval_dup(tr->tab[i]), elem_type);
                if (cval_is_error(e2)) {
                    v = BC_EXCEPTION;
                    goto done;
                }
                set_value(ctx, &tr1->tab[pos], e2);
                tensor_pos_incr(a_pos, n_dims, dims, 0);
            }
        }
        v = cnull_new(ctx);
    } else {
        if (n_rdims == 0) {
            /* for convenience, if zero output dimensions, return a scalar */
            pos = 0;
            for(j = 0; j < n_dims; j++)
                pos += strides[j] * offsets[j];
            v = cval_dup(tr1->tab[pos]);
        } else {
            v = tensor_new(ctx, v1->type->elem_type, n_rdims, rdims);
            tr = &v->u.tensor;
            
            /* get the slice */
            for(i = 0; i < n_dims; i++)
                a_pos[i] = 0;
            for(i = 0; i < tr->size; i++) {
                pos = 0;
                for(j = 0; j < n_dims; j++)
                    pos += strides[j] * (a_pos[j] + offsets[j]);
                set_value(ctx, &tr->tab[i], cval_dup(tr1->tab[pos]));
                tensor_pos_incr(a_pos, n_dims, dims, 0);
            }
        }
    }
    
 done:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return v;
}

static BCValue tensor_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    return tensor_getsetitem(ctx, n_args, args, FALSE);
}

static BCValue tensor_setitem(BCContext *ctx, int n_args, BCValue *args)
{
    return tensor_getsetitem(ctx, n_args, args, TRUE);
}

/*************************************************/
/* matrix (using tensors) */

#define EP(row, col, stride) ((row) * (stride) + (col))

/* matrix multiplication with implicit broadcast */
static BCValue matrix_mul(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCValue v = BC_EXCEPTION;
    BCTensor *tr, *tr1, *tr2;
    BCType *t = NULL;
    int rdims[MAX_DIMS], dims1[MAX_DIMS], dims2[MAX_DIMS];
    int n_dims, m, n, k, i, j, l, p, i1;
    
    t = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type,
                     BC_OP2_ADD);
    if (!t) 
        goto done;

    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;

    /* compute the dimensions of the result */
    n_dims = max_int(tr1->n_dims, tr2->n_dims);
    n_dims = max_int(n_dims, 2);
    p = 1;
    for(i = 0; i < n_dims; i++) {
        int d1, d2, d;
        if (i >= tr1->n_dims)
            d1 = 1;
        else
            d1 = tr1->dims[i];
        if (i >= tr2->n_dims)
            d2 = 1;
        else
            d2 = tr2->dims[i];
        if (i < 2) {
            dims1[i] = d1;
            dims2[i] = d2;
        } else {
            if (d1 == d2 || d2 == 1) {
                d = d1;
            } else if (d1 == 1) {
                d = d2;
            } else {
                cval_type_error(ctx, "incompatible tensor dimensions");
                goto done;
            }
            dims1[i] = d;
            dims2[i] = d;
            rdims[i] = d;
            p *= d;
        }
    }
    
    v1 = tensor_broadcast(ctx, v1, n_dims, dims1);
    if (cval_is_error(v1))
        goto done;
    v2 = tensor_broadcast(ctx, v2, n_dims, dims2);
    if (cval_is_error(v2))
        goto done;

    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;

    k = tr1->dims[0];
    m = tr1->dims[1];
    n = tr2->dims[0];

    if (k != tr2->dims[1]) {
        cval_type_error(ctx, "incompatible tensor dimensions for matrix multiplication");
        goto done;
    }
    rdims[0] = n;
    rdims[1] = m;

    v = tensor_new(ctx, t, n_dims, rdims);
    ctype_free(ctx, t);
    t = NULL;
    tr = &v->u.tensor;
    
    for(i1 = 0; i1 < p; i1++) {
        for(i = 0; i < m; i++) {
            for(j = 0; j < n; j++) {
                BCValue sum;
                sum = cint_from_int(ctx, 0);
                for(l = 0; l < k; l++) {
                    sum = cval_add(ctx, sum,
                                   cval_mul(ctx,
                                            cval_dup(tr1->tab[l + k * (i + m * i1)]),
                                            cval_dup(tr2->tab[j + n * (l + k * i1)])));
                }
                set_value(ctx, &tr->tab[j + n * (i + m * i1)], sum);
            }
        }
    }
 done:
    if (t)
        ctype_free(ctx, t);
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

/* return -1 if not square */
static int matrix_check_square(BCContext *ctx, BCValueConst v1)
{
    const BCTensor *tr;
    if (cval_type(v1) != CTYPE_TENSOR)
        goto fail;
    tr = &v1->u.tensor;
    if (tr->n_dims != 2 || tr->dims[0] != tr->dims[1]) {
    fail:
        cval_type_error(ctx, "square matrix expected");
        return -1;
    }
    return tr->dims[0];
}

static BCValue matrix_idn1(BCContext *ctx, int n, const BCType *elem_type)
{
    BCValue r;
    int i;
    BCTensor *tr;
    r = tensor_new_2d(ctx, elem_type, n, n);
    tr = &r->u.tensor;
    for(i = 0; i < n; i++) {
        set_value(ctx, &tr->tab[i*n+i], cval_convert(ctx, cint_from_int(ctx, 1), elem_type));
    }
    return r;
}

static BCValue matrix_idn(BCContext *ctx, BCValue arg)
{
    int n;
    if (cint_to_int(ctx, &n, arg)) {
        cval_free(ctx, arg);
        return BC_EXCEPTION;
    }
    cval_free(ctx, arg);
    if (n < 1) {
        return cval_type_error(ctx, "integer >= 1 expected");
    }
    return matrix_idn1(ctx, n, ctx->def_type[CTYPE_INT]);
}

static BCValue matrix_inverse(BCContext *ctx, BCValue v1)
{
    int n, i, j, k, n2;
    BCValue *dst, *src, r, v, c;
    BCType *t;
    
    n = matrix_check_square(ctx, v1);
    if (n < 0) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }
    n2 = n * n;
    src = mallocz(sizeof(src[0]) * n2);
    for(i = 0; i < n2; i++)
        src[i] = cval_dup(v1->u.tensor.tab[i]);
    t = get_inverse_type(ctx, v1->type->elem_type);
    if (!t) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }
    r = matrix_idn1(ctx, n, t);
    ctype_free(ctx, t);
    dst = r->u.tensor.tab;
    
    for(i=0;i<n;i++) {
        /* XXX: use max value instead */
        for(j = i; j < n; j++) {
            if (!cval_cmp_eq_int(ctx, src[EP(i, j, n)], 0))
                break;
        }
        if (j == n) {
            cval_free(ctx, r);
            r = cval_range_error(ctx, "matrix is not invertible");
            goto done;
        }
        if (j != i) {
            /* swap lines in src and dst */
            for(k = 0; k < n; k++) {
                v = src[EP(j, k, n)];
                src[EP(j, k, n)] = src[EP(i, k, n)];
                src[EP(i, k, n)] = v;
            }
            for(k = 0; k < n; k++) {
                v = dst[EP(j, k, n)];
                dst[EP(j, k, n)] = dst[EP(i, k, n)];
                dst[EP(i, k, n)] = v;
            }
        }

        c = cval_inverse(ctx, cval_dup(src[EP(i, i, n)]));
        for(k = 0; k < n; k++) {
            src[EP(i, k, n)] = cval_mul(ctx, src[EP(i, k, n)], cval_dup(c));
            dst[EP(i, k, n)] = cval_mul(ctx, dst[EP(i, k, n)], cval_dup(c));
        }
        cval_free(ctx, c);
        
        for(j = 0; j < n; j++) {
            if (j != i) {
                c = cval_dup(src[EP(j, i, n)]);
                for(k = i; k < n; k++) {
                    set_value(ctx, &src[EP(j, k, n)],
                              cval_sub(ctx, cval_dup(src[EP(j, k, n)]), 
                                       cval_mul(ctx, cval_dup(src[EP(i, k, n)]),
                                                cval_dup(c))));
                }
                for(k = 0; k < n; k++) {
                    set_value(ctx, &dst[EP(j, k, n)],
                              cval_sub(ctx, cval_dup(dst[EP(j, k, n)]), 
                                       cval_mul(ctx, cval_dup(dst[EP(i, k, n)]),
                                                cval_dup(c))));
                }
                cval_free(ctx, c);
            }
        }
    }
 done:
    for(i = 0; i < n2; i++)
        cval_free(ctx, src[i]);
    free(src);
    cval_free(ctx, v1);
    return r;
}

static BCValue matrix_diag(BCContext *ctx, BCValue v1)
{
    BCValue r;
    int n, i;
    BCTensor *tr;

    if (cval_type(v1) != CTYPE_TENSOR || v1->u.tensor.n_dims != 1) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "vector expected");
    }
    n = v1->u.tensor.dims[0];
    r = tensor_new_2d(ctx, v1->type->elem_type, n, n);
    tr = &r->u.tensor;
    for(i = 0; i < n; i++) {
        set_value(ctx, &tr->tab[i*n+i], cval_dup(v1->u.tensor.tab[i]));
    }
    cval_free(ctx, v1);
    return r;
}

/* hilbert matrix */
static BCValue mathilbert(BCContext *ctx, BCValue arg)
{
    BCValue r;
    int n, i, j;
    BCTensor *tr;
    if (cint_to_int(ctx, &n, arg)) {
        cval_free(ctx, arg);
        return BC_EXCEPTION;
    }
    cval_free(ctx, arg);
    if (n < 1) {
        return cval_type_error(ctx, "integer >= 1 expected");
    }
    r = tensor_new_2d(ctx, ctx->def_type[CTYPE_FRAC], n, n);
    tr = &r->u.tensor;
    for(i = 0; i < n; i++) {
        for(j = 0; j < n; j++) {
            set_value(ctx, &tr->tab[EP(i, j, n)],
                      cval_frac_div(ctx, cint_from_int(ctx, 1), cint_from_int(ctx, 1+i+j)));
        }
    }
    return r;
}

static BCValue matrix_trace(BCContext *ctx, BCValue v1)
{
    int n, i;
    BCValue r;
    BCTensor *tr;
    
    n = matrix_check_square(ctx, v1);
    if (n < 0) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }
    tr = &v1->u.tensor;
    r = cval_dup(tr->tab[0]);
    for(i=1;i<n;i++) {
        r = cval_add(ctx, r, cval_dup(tr->tab[i * n + i]));
    }
    cval_free(ctx, v1);
    return r;
}

static BCValue matrix_trans(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCTensor *tr, *tr1;
    int i, j, m, n;
    
    if (cval_type(v1) != CTYPE_TENSOR) {
        v = cval_type_error(ctx, "tensor expected");
        goto done;
    }
    tr1 = &v1->u.tensor;
    if (tr1->n_dims != 2 && tr1->n_dims != 1) {
        v = cval_type_error(ctx, "matrix or vector expected");
        goto done;
    }
    m = tr1->dims[0];
    if (tr1->n_dims >= 2)
        n = tr1->dims[1];
    else
        n = 1;

    v = tensor_new_2d(ctx, v1->type->elem_type, m, n);
    tr = &v->u.tensor;

    for(j = 0; j < n; j++) {
        for(i = 0; i < m; i++) {
            set_value(ctx, &tr->tab[j + i * n], cval_dup(tr1->tab[i + j * m]));
        }
    }
 done:
    cval_free(ctx, v1);
    return v;
}

static BCValue matrix_charpoly(BCContext *ctx, BCValue v1)
{
    int n, i, j;
    BCValue c, v, coef;
    BCPoly *p;
    BCType *elem_type;
    BOOL is_int;
    n = matrix_check_square(ctx, v1);
    if (n < 0) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }
    elem_type = v1->type->elem_type;
    is_int = is_complex_int(elem_type);
    v = poly_new(ctx, elem_type, n + 1);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
    coef = cval_convert(ctx, cint_from_int(ctx, 1), elem_type);
    set_value(ctx, &p->tab[n], coef);
    c = matrix_idn1(ctx, n, elem_type);
    for(i = 0; i < n; i++) {
        c = cval_mul(ctx, c, cval_dup(v1));
        coef = cval_neg(ctx, matrix_trace(ctx, cval_dup(c)));
        if (is_int)
            coef = cval_divexact(ctx, coef, cint_from_int(ctx, i + 1));
        else
            coef = cval_div(ctx, coef, cint_from_int(ctx, i + 1));
        set_value(ctx, &p->tab[n - i - 1], cval_dup(coef));
        for(j = 0; j < n; j++) {
            set_value(ctx, &c->u.tensor.tab[j * n + j],
                      cval_add(ctx, cval_dup(c->u.tensor.tab[j * n + j]),
                               cval_dup(coef)));
        }
        cval_free(ctx, coef);
    }
    cval_free(ctx, c);
 done:
    cval_free(ctx, v1);
    return v;
}

static BCValue vector_dp(BCContext *ctx, BCValue v1, BCValue v2)
{
    int i, n;
    BCTensor *tr1, *tr2;
    BCValue v;

    if (cval_type(v1) != CTYPE_TENSOR || cval_type(v2) != CTYPE_TENSOR) {
        v = cval_type_error(ctx, "tensors expected");
        goto done;
    }
    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;
    if (tr1->n_dims != 1 || tr2->n_dims != 1 || tr1->dims[0] != tr2->dims[0]) {
        v = cval_type_error(ctx, "single dimension tensors expected");
        goto done;
    }
    n = tr1->dims[0];
    v = cint_from_int(ctx, 0);
    for(i = 0; i < n; i++) {
        v = cval_add(ctx, v, cval_mul(ctx, cval_dup(tr1->tab[i]),
                                      cval_dup(tr2->tab[i])));
    }
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue vector_cp(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCTensor *tr1, *tr2, *tr;
    BCType *elem_type;
    BCValue v;

    if (cval_type(v1) != CTYPE_TENSOR || cval_type(v2) != CTYPE_TENSOR) {
        v = cval_type_error(ctx, "tensors expected");
        goto done;
    }
    tr1 = &v1->u.tensor;
    tr2 = &v2->u.tensor;
    if (tr1->n_dims != 1 || tr2->n_dims != 1 || tr1->dims[0] != tr2->dims[0] ||
        tr1->dims[0] != 3) {
        v = cval_type_error(ctx, "3 dimension tensors expected");
        goto done;
    }
    elem_type = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type,
                             BC_OP2_ADD);
    if (!elem_type) {
        v = BC_EXCEPTION;
        goto done;
    }
    v = tensor_new(ctx, elem_type, 1, tr1->dims);
    ctype_free(ctx, elem_type);
    tr = &v->u.tensor;
    set_value(ctx, &tr->tab[0],
              cval_sub(ctx, cval_mul(ctx, cval_dup(tr1->tab[1]), cval_dup(tr2->tab[2])),
                       cval_mul(ctx, cval_dup(tr1->tab[2]), cval_dup(tr2->tab[1]))));
    set_value(ctx, &tr->tab[1],
              cval_sub(ctx, cval_mul(ctx, cval_dup(tr1->tab[2]), cval_dup(tr2->tab[0])),
                       cval_mul(ctx, cval_dup(tr1->tab[0]), cval_dup(tr2->tab[2]))));
    set_value(ctx, &tr->tab[2],
              cval_sub(ctx, cval_mul(ctx, cval_dup(tr1->tab[0]), cval_dup(tr2->tab[1])),
                       cval_mul(ctx, cval_dup(tr1->tab[1]), cval_dup(tr2->tab[0]))));
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue matrix_eigenvals(BCContext *ctx, BCValue v1)
{
    v1 = matrix_charpoly(ctx, v1);
    return poly_roots(ctx, 1, &v1);
}

/* XXX: should keep integer/polynomial result */
static BCValue matrix_det(BCContext *ctx, BCValue v1)
{
    int n, n2, i, j, k, s;
    BCValue *src, c, v;

    n = matrix_check_square(ctx, v1);
    if (n < 0) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }
    s = 1;
    n2 = n * n;
    src = mallocz(sizeof(src[0]) * n2);
    for(i = 0; i < n2; i++)
        src[i] = cval_dup(v1->u.tensor.tab[i]);
    for(i=0;i<n;i++) {
        /* XXX: should take the largest value if float */
        for(j = i; j < n; j++) {
            if (!cval_cmp_eq_int(ctx, src[j * n + i], 0))
                break;
        }
        if (j == n) {
            c = cint_from_int(ctx, 0);
            goto done;
        }
        if (j != i) {
            for(k = 0; k < n; k++) {
                v = src[j * n + k];
                src[j * n + k] = src[i * n + k];
                src[i * n + k] = v;
            }
            s = -s;
        }
        c = cval_inverse(ctx, cval_dup(src[i * n + i]));
        if (cval_is_error(c)) {
            goto done;
        }
        for(j = i + 1; j < n; j++) {
            v = cval_mul(ctx, cval_dup(c), cval_dup(src[j * n + i]));
            for(k = 0;k < n; k++) {
                src[j * n + k] =
                    cval_sub(ctx, src[j * n + k], 
                             cval_mul(ctx, cval_dup(src[i * n + k]),
                                      cval_dup(v)));
            }
            cval_free(ctx, v);
        }
        cval_free(ctx, c);
    }
    c = cint_from_int(ctx, s);
    for(i=0;i<n;i++)
        c = cval_mul(ctx, c, cval_dup(src[i * n + i]));
 done:
    for(i = 0; i < n2; i++)
        cval_free(ctx, src[i]);
    free(src);
    cval_free(ctx, v1);
    return c;
}

static BCValue matrix_rank_ker(BCContext *ctx, BCValue v1, BOOL is_ker)
{
    BCTensor *tr;
    int i, j, k, w, h, l;
    BCValue *src, c, r;
    BCType *elem_type;
    uint8_t *im_cols;
    
    if (cval_type(v1) != CTYPE_TENSOR ||
        v1->u.tensor.n_dims != 2) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "matrix expected");
    }
    elem_type = v1->type->elem_type;
    if (is_complex_frac(elem_type)) {
        /* OK */
        elem_type = ctype_dup(elem_type);
    } else if (is_complex_int(elem_type)) {
        /* convert to frac */
        elem_type = get_inverse2_type(ctx, elem_type, TRUE);
        v1 = tensor_convert(ctx, v1, elem_type);
    } else {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "rational matrix expected");
    }
    tr = &v1->u.tensor;
    w = tr->dims[0];
    h = tr->dims[1];
    l = w * h;
    src = mallocz(sizeof(src[0]) * l);
    for(i = 0; i < l; i++)
        src[i] = cval_dup(tr->tab[i]);
    if (is_ker)
        im_cols = mallocz(sizeof(im_cols[0]) * w);
    else
        im_cols = NULL;
    l = 0;
    for(i=0;i<w;i++) {
        for(j = l; j < h; j++) {
            if (!cval_cmp_eq_int(ctx, src[j * w + i], 0))
                break;
        }
        if (j == h)
            continue;
        if (im_cols)
            im_cols[i] = TRUE;
        if (j != l) {
            /* swap lines */
            for(k = 0; k < w; k++) {
                BCValue v = src[j * w + k];
                src[j * w + k] = src[l * w + k];
                src[l * w + k] = v;
            }
        }
        c = cval_inverse(ctx, cval_dup(src[l * w + i]));
        for(k = 0; k < w; k++) {
            src[l * w + k] = cval_mul(ctx, src[l * w + k], cval_dup(c));
        }
        cval_free(ctx, c);
        
        for(j = is_ker ? 0 : l + 1; j < h; j++) {
            if (j != l) {
                c = cval_dup(src[j * w + i]);
                for(k = i; k < w; k++) {
                    src[j * w + k] =
                        cval_sub(ctx, src[j * w + k], 
                                 cval_mul(ctx, cval_dup(src[l * w + k]),
                                          cval_dup(c)));
                }
                cval_free(ctx, c);
            }
        }
        l++;
        //        log_str("m=" + cval_toString(v1) + "\n");
    }
    if (is_ker) {
        int m, ker_dim = w - l;
        r = tensor_new_2d(ctx, elem_type, w, ker_dim);
        tr = &r->u.tensor;
        k = 0;
        for(i = 0; i < w; i++) {
            if (!im_cols[i]) {
                /* select this column from the matrix */
                l = 0;
                m = 0;
                for(j = 0; j < w; j++) {
                    if (im_cols[j]) {
                        c = cval_neg(ctx, cval_dup(src[m * w + i]));
                        m++;
                    } else {
                        c = cval_convert(ctx, cint_from_int(ctx, (l == k)),
                                         elem_type);
                        l++;
                    }
                    set_value(ctx, &tr->tab[j * ker_dim + k], c);
                }
                k++;
            }
        }
        /* XXX: could use integer coordinates when needed */
    } else {
        r = cint_from_int(ctx, l);
    }
    free(im_cols);
    for(i = 0; i < h * w; i++)
        cval_free(ctx, src[i]);
    ctype_free(ctx, elem_type);
    free(src);
    cval_free(ctx, v1);
    return r;
}

static BCValue matrix_rank(BCContext *ctx, BCValue v1)
{
    return matrix_rank_ker(ctx, v1, FALSE);
}

static BCValue matrix_ker(BCContext *ctx, BCValue v1)
{
    return matrix_rank_ker(ctx, v1, TRUE);
}

/*************************************************/
/* polynomial */

static BOOL is_poly_elem_type(int tag)
{
    switch(tag) {
    case CTYPE_INT:
    case CTYPE_FRAC:
    case CTYPE_DECIMAL:
    case CTYPE_FLOAT:
    case CTYPE_COMPLEX:
        return TRUE;
        break;
    default:
        return FALSE;
    }
}

static BCValue poly_new(BCContext *ctx, const BCType *elem_type, int len)
{
    BCType *t;
    BCValue v;
    BCPoly *p;
    int i;
    
    assert(len >= 1);

    if (!is_poly_elem_type(elem_type->tag))  {
        return cval_type_error(ctx, "only numeric types are allowed in polynomials");
    }
    
    t = ctype_new(ctx, CTYPE_POLY, elem_type);
    v = cval_new1(ctx, t);
    ctype_free(ctx, t);
    p = &v->u.poly;
    p->len = len;
    p->emin = 0;
    p->tab = malloc(sizeof(p->tab[0]) * len);
    for(i = 0; i < len; i++) {
        p->tab[i] = cval_convert(ctx, cint_from_int(ctx, 0), elem_type);
    }
    return v;
}

static BCValue poly_ctor(BCContext *ctx, BCValue v1)
{
    int i, len;
    BCType *t = NULL;
    BCValue v;
    BCArray *arr = NULL;
    BCTensor *tr = NULL;
    BCPoly *p;
    
    if (cval_type(v1) == CTYPE_ARRAY) {
        arr = &v1->u.array;
        len = arr->len;
        if (len == 0) {
            v = cval_type_error(ctx, "at least one element expected");
            goto done;
        }
        for(i = 0; i < len; i++) {
            BCValue e1 = arr->tab[i];
            if (!t) {
                t = ctype_dup(e1->type);
            } else {
                BCType *t1;
                t1 = get_op2_type(ctx, t, e1->type, BC_OP2_ADD);
                if (!t1) {
                    v = BC_EXCEPTION;
                    goto done;
                }
                ctype_free(ctx, t);
                t = t1;
            }
        }
    } else if (cval_type(v1) == CTYPE_TENSOR) {
        tr = &v1->u.tensor;
        if (tr->n_dims != 1) {
            v = cval_type_error(ctx, "tensor of dimension 1 expected");
            goto done;
        }
        len = tr->dims[0];
        t = ctype_dup(v1->type->elem_type);
    } else {
        t = ctype_dup(v1->type);
        arr = NULL;
        len = 1;
    }
    
    v = poly_new(ctx, t, len);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
    if (cval_type(v1) == CTYPE_ARRAY) {
        for(i = 0; i < len; i++) {
            BCValue e1 = cval_dup(arr->tab[i]);
            e1 = cval_convert(ctx, e1, t);
            if (cval_is_error(e1)) {
                cval_free(ctx, v);
                goto done;
            }
            set_value(ctx, &p->tab[i], e1);
        }
    } else if (cval_type(v1) == CTYPE_TENSOR) {
        for(i = 0; i < len; i++) {
            BCValue e1 = cval_dup(tr->tab[i]);
            set_value(ctx, &p->tab[i], e1);
        }
    } else {
        set_value(ctx, &p->tab[0], cval_dup(v1));
    }
 done:
    if (t)
        ctype_free(ctx, t);
    cval_free(ctx, v1);
    return v;
}

static BCValue poly_new_X(BCContext *ctx)
{
    BCValue v;
    v = poly_new(ctx, ctx->def_type[CTYPE_INT], 2);
    set_value(ctx, &v->u.poly.tab[1], cint_from_int(ctx, 1));
    return v;
}

static BCValue poly_new2(BCContext *ctx, const BCType *t1, const BCType *t2,
                         int len)
{
    BCType *t;
    BCValue v;
    
    t = get_op2_type(ctx, t1, t2, BC_OP2_ADD);
    if (!t)
        return BC_EXCEPTION;
    v = poly_new(ctx, t, len);
    ctype_free(ctx, t);
    return v;
}

static void poly_trim(BCContext *ctx, BCValue v1)
{
    BCPoly *p = &v1->u.poly;
    int i, new_len;
    i = p->len;
    while (i > 1 && cval_cmp_eq_int(ctx, p->tab[i - 1], 0))
        i--;
    if (i == p->len)
        return;
    new_len = i;
    for(i = new_len; i < p->len; i++)
        cval_free(ctx, p->tab[i]);
    p->tab = realloc(p->tab, sizeof(p->tab[0]) * new_len);
    p->len = new_len;
}

static BCValue to_poly(BCContext *ctx, BCValue v1)
{
    BCValue v;
    if (cval_type(v1) == CTYPE_POLY)
        return v1;
    if (!is_poly_elem_type(cval_type(v1))) {
        v = cval_type_error(ctx, "cannot convert to polynomial");
        goto fail;
    }
    v = poly_new(ctx, v1->type, 1);
    if (cval_is_error(v)) {
    fail:
        cval_free(ctx, v1);
        return v;
    }
    set_value(ctx, &v->u.poly.tab[0], v1);
    return v;
}

static BCValue poly_convert(BCContext *ctx, BCValue v1, const BCType *elem_type)
{
    BCValue v, e1;
    BCPoly *p, *p1;
    int i;

    p1 = &v1->u.poly;
    v = poly_new(ctx, elem_type, p1->len);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
    for(i = 0; i < p1->len; i++) {
        e1 = cval_convert(ctx, cval_dup(p1->tab[i]), elem_type);
        if (cval_is_error(e1)) {
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &p->tab[i], e1);
    }
    poly_trim(ctx, v);
 done:
    cval_free(ctx, v1);
    return v;
}


static BCValue poly_add(BCContext *ctx, BCValue v1, BCValue v2, BOOL v2_neg)
{
    BCPoly *p1, *p2, *p;
    BCValue v, e1;
    int i, n_min, n_max;
    BOOL v2_is_longer;
    p1 = &v1->u.poly;
    p2 = &v2->u.poly;
    if (p2->len > p1->len) {
        n_min = p1->len;
        n_max = p2->len;
        v2_is_longer = TRUE;
    } else {
        n_min = p2->len;
        n_max = p1->len;
        v2_is_longer = FALSE;
    }
    v = poly_new2(ctx, v1->type->elem_type, v2->type->elem_type, n_max);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
        
    for(i = 0; i < n_min; i++) {
        if (v2_neg)
            e1 = cval_sub(ctx, cval_dup(p1->tab[i]), cval_dup(p2->tab[i]));
        else
            e1 = cval_add(ctx, cval_dup(p1->tab[i]), cval_dup(p2->tab[i]));
        if (cval_is_error(e1))
            goto fail;
        set_value(ctx, &p->tab[i], e1);
    }
    for(i = n_min; i < n_max; i++) {
        if (v2_is_longer) {
            e1 = cval_dup(p2->tab[i]);
            if (v2_neg) {
                e1 = cval_neg(ctx, e1);
                if (cval_is_error(e1))
                    goto fail;
            }
        } else {
            e1 = cval_dup(p1->tab[i]);
        }
        e1 = cval_convert(ctx, e1, v->type->elem_type);
        if (cval_is_error(e1)) {
        fail:
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &p->tab[i], e1);
    }

    poly_trim(ctx, v);
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue poly_mul(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCPoly *p1, *p2, *p;
    BCValue v;
    int i, j;

    p1 = &v1->u.poly;
    p2 = &v2->u.poly;
    v = poly_new2(ctx, v1->type->elem_type, v2->type->elem_type, p1->len + p2->len - 1);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
    for(i = 0; i < p1->len; i++) {
        for(j = 0; j < p2->len; j++) {
            set_value(ctx, &p->tab[i + j],
                      cval_add(ctx, cval_dup(p->tab[i + j]),
                               cval_mul(ctx, cval_dup(p1->tab[i]),
                                        cval_dup(p2->tab[j]))));
        }
    }
    
    poly_trim(ctx, v);
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue poly_div_const(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCPoly *p1, *p2, *p;
    BCValue v, e1;
    int i;

    p1 = &v1->u.poly;
    p2 = &v2->u.poly;
    if (p2->len != 1) {
        v = cval_type_error(ctx, "polynomial divisor must be a constant");
        goto done;
    }
    v = poly_new2(ctx, v1->type->elem_type, v2->type->elem_type, p1->len);
    if (cval_is_error(v))
        goto done;

    p = &v->u.poly;
    for(i = 0; i < p1->len; i++) {
        e1 = cval_div(ctx, cval_dup(p1->tab[i]), cval_dup(p2->tab[0]));
        if (cval_is_error(e1)) {
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &p->tab[i], e1);
    }
    poly_trim(ctx, v);
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue poly_cmp_eq(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCPoly *p1, *p2;
    int i;
    BOOL res;
    
    p1 = &v1->u.poly;
    p2 = &v2->u.poly;
    if (p1->len != p2->len) {
        res = FALSE;
        goto done;
    }
    for(i = 0; i < p1->len; i++) {
        res = cval_cmp_eq2(ctx, p1->tab[i], p2->tab[i]);
        if (!res)
            goto done;
    }
    res = TRUE;
 done:
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return cbool_new(ctx, res);
}

static BCValue poly_divrem(BCContext *ctx, BCValue *pq1, BCValue v1, BCValue v2)
{
    BCPoly *p1, *p2, *pr, *pq;
    BCType *t;
    BCValue r, q, c;
    int i, j, n1, n2;
    BOOL is_int;
    
    p1 = &v1->u.poly;
    p2 = &v2->u.poly;

    q = BC_EXCEPTION;
    r = BC_EXCEPTION;
    t = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type, BC_OP2_ADD);
    if (!t) 
        goto done;
    n1 = p1->len;
    n2 = p2->len;
    if (n1 < n2) {
        r = cval_dup(v1);
        q = poly_new(ctx, t, 1);
        goto done;
    }
    r = poly_new(ctx, t, n1);
    if (cval_is_error(r))
        goto done;
    pr = &r->u.poly;
    for(i = 0; i < n1; i++)
        set_value(ctx, &pr->tab[i], cval_dup(p1->tab[i]));
    q = poly_new(ctx, t, n1 - n2 + 1);
    if (cval_is_error(q)) {
        cval_free(ctx, r);
        r = BC_EXCEPTION;
        goto done;
    }
    pq = &q->u.poly;

    is_int = (t->tag == CTYPE_INT ||
              (t->tag == CTYPE_COMPLEX && t->elem_type->tag == CTYPE_INT));

    for(i = n1 - n2; i >= 0; i--) {
        c = cval_dup(pr->tab[i + n2 - 1]);
        if (!cval_cmp_eq_int(ctx, c, 0)) {
            if (is_int) {
                c = cval_divexact(ctx, c, cval_dup(p2->tab[n2 - 1]));
            } else {
                c = cval_div(ctx, c, cval_dup(p2->tab[n2 - 1]));
            }
            if (cval_is_error(c)) {
                q = BC_EXCEPTION;
                r = BC_EXCEPTION;
                goto done;
            }
            for(j = 0; j < n2; j++) {
                set_value(ctx, &pr->tab[i + j],
                          cval_sub(ctx, cval_dup(pr->tab[i + j]),
                                   cval_mul(ctx, cval_dup(p2->tab[j]),
                                            cval_dup(c))));
            }
        }
        set_value(ctx, &pq->tab[i], c);
    }
    poly_trim(ctx, q);
    poly_trim(ctx, r);
 done:
    if (t)
        ctype_free(ctx, t);
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    if (pq1)
        *pq1 = q;
    else
        cval_free(ctx, q);
    return r;
}

static BCValue poly_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_poly(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_poly(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    switch(op) {
    case BC_OP2_ADD:
        return poly_add(ctx, v1, v2, FALSE);
    case BC_OP2_SUB:
        return poly_add(ctx, v1, v2, TRUE);
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        return poly_mul(ctx, v1, v2);
    case BC_OP2_DIV:
        return poly_div_const(ctx, v1, v2);
    case BC_OP2_MOD:
        return poly_divrem(ctx, NULL, v1, v2);
    case BC_OP2_DIVREM:
        {
            BCValue q, r;
            r = poly_divrem(ctx, &q, v1, v2);
            if (cval_is_error(q)) {
                v = BC_EXCEPTION;
            } else {
                v = carray_pair(ctx, q, r);
            }
            return v;
        }
        break;
    case BC_OP2_CMP_EQ:
        return poly_cmp_eq(ctx, v1, v2);
    case BC_OP2_FRAC_DIV:
        return rfrac_new(ctx, v1, v2);
    default:
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static int poly_deg(BCContext *ctx, BCValueConst v1)
{
    int n;
    assert(cval_type(v1) == CTYPE_POLY);
    n = v1->u.poly.len - 1;
    if (n == 0 && cval_cmp_eq_int(ctx, v1->u.poly.tab[0], 0)) {
        n = -1;
    }
    return n;
}

static BCValue poly_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue v, val;
    BCPoly *p;
    int i;

    if (n_args != 2) {
        val = cval_type_error(ctx, "polynomials have a single dimension");
        goto done;
    }
    v = args[0];
    p = &v->u.poly;
    if (cval_type(args[1]) == CTYPE_RANGE) {
        val = cval_type_error(ctx, "ranges are not supported for polynomials");
        goto done;
    } else {
        int idx;
        if (cint_to_int(ctx, &idx, args[1])) {
            val = BC_EXCEPTION;
            goto done;
        }
        if (idx < 0)
            idx += p->len;
        if (idx < 0 || idx >= p->len) {
            val = cval_range_error(ctx, "index out of bounds");
            goto done;
        }
        val = cval_dup(p->tab[idx]);
    }
 done:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return val;
}

static BCValue poly_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    switch(op) {
    case BC_OP1_NEG:
        return cval_mul(ctx, v1, cint_from_int(ctx, -1));
    default:
        cval_free(ctx, v1);
        return cval_type_error(ctx, "unsupported type");
    }
}

static BCValue cval_deg(BCContext *ctx, BCValue v1)
{
    BCValue v;
    if (cval_type(v1) != CTYPE_POLY) {
        v = cval_type_error(ctx, "polynomial expected");
        goto done;
    }
    v = cint_from_int(ctx, poly_deg(ctx, v1));
 done:
    cval_free(ctx, v1);
    return v;
}

static void monomial_toString(BCContext *ctx, DynBuf *d, BCValueConst c, int i)
{
    if (i == 0) {
        cval_toString(ctx, d, c);
    } else {
        if (cval_type(c) == CTYPE_INT && cval_cmp_eq_int(ctx, c, 1)) {
        } else if (cval_type(c) == CTYPE_INT && cval_cmp_eq_int(ctx, c, -1)) {
            dbuf_putc(d, '-');
        } else {
            BOOL need_paren = cval_type(c) == CTYPE_COMPLEX;
            if (need_paren)
                dbuf_putc(d, '(');
            cval_toString(ctx, d, c);
            if (need_paren)
                dbuf_putc(d, ')');
            dbuf_putc(d, '*');
        }
        dbuf_putc(d, 'X');
        if (i < 0 || i >= 2) {
            dbuf_printf(d, "^%d", i);
        }
    }
}

static void poly_toString(BCContext *ctx, DynBuf *d, BCValueConst  v1)
{
    int i, pos;
    BCValue c;
    const BCPoly *p;
    BOOL is_first;
    
    is_first = TRUE;
    p = &v1->u.poly;
    for(i = p->len - 1; i >= 0; i--) {
        c = p->tab[i];
        if (!cval_cmp_eq_int(ctx, c, 0) &&
            (i >= 1 || (i == 0 && !is_first))) {
            pos = d->size;
            monomial_toString(ctx, d, c, i);
            if (d->buf[pos] != '-' && !is_first) {
                insert_plus(d, pos);
            }
            is_first = FALSE;
        }
    }
    
    if (is_first) {
        dbuf_putstr(d, "Polynomial(");
        cval_toString(ctx, d, p->tab[0]);
        dbuf_putstr(d, ")");
    }
}

static BCValue poly_apply(BCContext *ctx, BCValue func_val, BCValue x)
{
    BCValue v;
    BCPoly *p = &func_val->u.poly;
    int n;

    n = p->len - 1;
    v = cval_dup(p->tab[n]);
    while (n > 0) {
        n--;
        v = cval_add(ctx, cval_mul(ctx, v, cval_dup(x)), cval_dup(p->tab[n]));
    }
    cval_free(ctx, func_val);
    cval_free(ctx, x);
    return v;
}

/* pseudo remainder for integer or complex(int) polynomials */
static BCValue poly_prem(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCPoly *p2;
    int a, b;
    
    p2 = &v2->u.poly;
    a = poly_deg(ctx, v1);
    b = poly_deg(ctx, v2);
    assert(b >= 0);
    if (a >= b) {
        v1 = cval_mul(ctx, v1, cval_pow(ctx, cval_dup(p2->tab[p2->len - 1]),
                                        cint_from_int(ctx, a - b + 1)));
    }
    return poly_divrem(ctx, NULL, v1, v2);
}

static BOOL is_integer_poly(BCValueConst v1)
{
    return (cval_type(v1) == CTYPE_POLY && v1->type->elem_type->tag == CTYPE_INT);
}

/* gcd of the coefficients of the integer polynomial */
static BCValue poly_cont(BCContext *ctx, BCValue v1)
{
    BCPoly *p1;
    BCValue g;
    int i;

    if (!is_integer_poly(v1)) {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "integer polynomial expected");
    }
    p1 = &v1->u.poly;
    g = cval_dup(p1->tab[0]);
    for(i = 1; i < p1->len; i++)
        g = cval_gcd(ctx, g, cval_dup(p1->tab[i]));
    cval_free(ctx, v1);
    return g;
}

/* primitive part of a poly */
static BCValue poly_primpart(BCContext *ctx, BCValue v1)
{
    BCValue g;
    g = poly_cont(ctx, cval_dup(v1));
    if (cval_is_error(g)) {
        cval_free(ctx, v1);
        return g;
    }
    /* in order to get an unique result, we force the leading term to be positive */
    if (cval_cmp_lt_int(ctx, v1->u.poly.tab[v1->u.poly.len - 1], 0) != cval_cmp_lt_int(ctx, g, 0)) {
        g = cval_neg(ctx, g);
    }
    return cval_divexact(ctx, v1, g);
}

/* Note: the result is always poly(frac) or poly(complex(frac)) */
/* XXX: return an integer result for integer inputs */
static BCValue poly_gcd(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCPoly *p1;
    BCType *t;
    BOOL is_int;
    
    t = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type, BC_OP2_ADD);
    if (!t) {
        cval_free(ctx, v1);
        cval_free(ctx, v2);
        return BC_EXCEPTION;
    }
    /* XXX: don't convert complex(int) to complex(frac) */
    is_int = (t->tag == CTYPE_INT);
    if (!is_int) {
        ctype_free(ctx, t);
        t = get_op2_type(ctx, v1->type->elem_type, v2->type->elem_type, BC_OP2_ADD);
        if (!t) {
            cval_free(ctx, v1);
            cval_free(ctx, v2);
            return BC_EXCEPTION;
        }
    }
    t = ctype_new_free(ctx, CTYPE_POLY, t);
    
    v1 = cval_convert(ctx, v1, t);
    if (cval_is_error(v1)) {
        ctype_free(ctx, t);
        cval_free(ctx, v2);
        return BC_EXCEPTION;
    }
    v2 = cval_convert(ctx, v2, t);
    ctype_free(ctx, t);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return BC_EXCEPTION;
    }

    while (poly_deg(ctx, v2) >= 0) {
        BCValue tmp = cval_dup(v2);
        if (is_int) {
            v2 = poly_prem(ctx, v1, v2);
            v2 = poly_primpart(ctx, v2);
        } else {
            v2 = cval_mod(ctx, v1, v2);
        }
        //        cval_dump(ctx, "r", v2);
        if (cval_is_error(v2)) {
            cval_free(ctx, tmp);
            return BC_EXCEPTION;
        }
        v1 = tmp;
    }
    cval_free(ctx, v2);
    /* convert to monic form */
    if (!is_int) {
        p1 = &v1->u.poly;          
        v1 = cval_div(ctx, v1, cval_dup(p1->tab[p1->len - 1]));
    }
    return v1;
}

static BCValue poly_deriv(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCPoly *p1, *p;
    int i, n;
    
    p1 = &v1->u.poly;
    n = p1->len;
    v = poly_new(ctx, v1->type->elem_type, max_int(n - 1, 1));
    p = &v->u.poly;
    for(i = 1; i < n; i++) {
        set_value(ctx, &p->tab[i - 1],
                  cval_mul(ctx, cval_dup(p1->tab[i]), cint_from_int(ctx, i)));
    }
    cval_free(ctx, v1);
    poly_trim(ctx, v);
    return v;
}

static BCValue poly_integ(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCPoly *p1, *p;
    BCType *elem_type;
    int i, n;
    
    p1 = &v1->u.poly;
    n = p1->len;
    elem_type = get_inverse_type(ctx, v1->type->elem_type);
    v = poly_new(ctx, elem_type, n + 1);
    ctype_free(ctx, elem_type);
    p = &v->u.poly;
    for(i = 0; i < n; i++) {
        set_value(ctx, &p->tab[i + 1],
                  cval_div(ctx, cval_dup(p1->tab[i]), cint_from_int(ctx, i + 1)));
    }
    cval_free(ctx, v1);
    poly_trim(ctx, v);
    return v;
}

/* find one complex root of 'p' starting from z at precision 'eps'
   using at most max_it iterations. Return null if could not find
   root. 'p' must be of type complex(float) or complex(decimal). */
static BCValue poly_root_laguerre1(BCContext *ctx, BCValueConst p,
                                   BCValueConst initial_z, BCValueConst eps,
                                   int max_it)
{
    BCValue p1, p2, z0, z1, z2, t0, t1, d1, d2, eps2, z;
    int i, d;
    BOOL res;
    
    d = p->u.poly.len - 1;
    assert(d >= 1);
    if (d == 1) {
        /* monomial case */
        return cval_neg(ctx, cval_div(ctx, cval_dup(p->u.poly.tab[0]), cval_dup(p->u.poly.tab[1])));
    }
    z = cval_dup(initial_z);
    eps2 = cval_norm2(ctx, cval_dup(eps));
    p1 = poly_deriv(ctx, cval_dup(p));
    p2 = poly_deriv(ctx, cval_dup(p1));
    for(i = 0; i < max_it; i++) {
        z0 = poly_apply(ctx, cval_dup(p), cval_dup(z));
        if (cval_is_error(z0)) {
            cval_free(ctx, z);
            z = BC_EXCEPTION;
            goto done;
        }

        t0 = cval_norm2(ctx, cval_dup(z0));
        res = cval_cmp_le2(ctx, t0, eps2);
        cval_free(ctx, t0);
        if (res) { 
            cval_free(ctx, z0);
            goto done;
        }
        
        z1 = poly_apply(ctx, cval_dup(p1), cval_dup(z));
        z2 = poly_apply(ctx, cval_dup(p2), cval_dup(z));
        t0 = cval_mul(ctx, cint_from_int(ctx, d - 1), cval_dup(z1));
        t0 = cval_mul(ctx, cval_dup(t0), t0);
        t1 = cval_mul(ctx, cval_mul(ctx, cval_mul(ctx, cint_from_int(ctx, d),
                                                  cint_from_int(ctx, d - 1)), cval_dup(z0)), z2);
        t0 = cval_sqrt(ctx, cval_sub(ctx, t0, t1));

        d1 = cval_add(ctx, cval_dup(z1), cval_dup(t0));
        d2 = cval_sub(ctx, z1, t0);
        t0 = cval_norm2(ctx, cval_dup(d1));
        t1 = cval_norm2(ctx, cval_dup(d2));
        res = cval_cmp_lt2(ctx, t0, t1);
        cval_free(ctx, t0);
        cval_free(ctx, t1);
        if (res) {
            set_value(ctx, &d1, d2);
        } else {
            cval_free(ctx, d2);
        }
        
        if (cval_cmp_eq_int(ctx, d1, 0))  {
            cval_free(ctx, z0);
            cval_free(ctx, d1);
            cval_free(ctx, z);
            z = cval_range_error(ctx, "root not found");
            goto done;
        }
        z = cval_sub(ctx, z, cval_div(ctx, cval_mul(ctx, cint_from_int(ctx, d), z0), d1));
    }
 done:
    cval_free(ctx, eps2);
    cval_free(ctx, p1);
    cval_free(ctx, p2);
    return z;
}

static BCValue poly_roots(BCContext *ctx, int n_args, BCValue *args)
{
    static const double start_points[] = { 0.1, -1.4, 1.7 };
    int d, i, j;
    BCValue roots = BC_EXCEPTION, a, z, r, eps, p, pd;
    BCType *elem_type = NULL;
    
    p = args[0];
    if (n_args >= 2) {
        eps = args[1];
    } else {
        eps = cfloat_from_double(ctx, 1e-10);
    }
    if (cval_type(p) != CTYPE_POLY) {
        cval_type_error(ctx, "polynomial expected");
        goto done;
    }
    d = p->u.poly.len - 1;
    if (d == 0) {
        cval_range_error(ctx, "polynomial of degree >= 1 expected");
        goto done;
    }
    /* convert to Complex(Decimal) or Complex(Float) */
    elem_type = p->type->elem_type;
    if (elem_type->tag == CTYPE_FLOAT || (elem_type->tag == CTYPE_COMPLEX && elem_type->elem_type->tag == CTYPE_FLOAT)) {
        elem_type = ctx->def_type[CTYPE_FLOAT];
    } else {
        elem_type = ctx->def_type[CTYPE_DECIMAL];
    }
    eps = cval_convert(ctx, eps, elem_type);
    elem_type = ctype_new(ctx, CTYPE_COMPLEX, elem_type);
    p = poly_convert(ctx, p, elem_type);
    if (!p)
        goto done;

    roots = tensor_new(ctx, elem_type, 1, &d);
    for(i = 0; i < d; i++) {
        /* XXX: should select another start point if error */
        for(j = 0; j < countof(start_points); j++) {
            a = cval_convert(ctx, cfloat_from_double(ctx, start_points[j]), elem_type);
            z = poly_root_laguerre1(ctx, p, a, eps, 50);
            cval_free(ctx, a);
            if (!cval_is_error(z))
                break;
        }
        if (cval_is_error(z)) {
            cval_free(ctx, roots);
            roots = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &roots->u.tensor.tab[i], cval_dup(z));
        pd = poly_new(ctx, elem_type, 2);
        set_value(ctx, &pd->u.poly.tab[1], cval_convert(ctx, cint_from_int(ctx, 1), elem_type));
        set_value(ctx, &pd->u.poly.tab[0], cval_neg(ctx, z));
        r = poly_divrem(ctx, &p, p, pd);
        cval_free(ctx, r);
    }
 done:
    if (elem_type)
        ctype_free(ctx, elem_type);
    cval_free(ctx, p);
    cval_free(ctx, eps);
    for(i = 2; i < n_args; i++)
        cval_free(ctx, args[i]);
    return roots;
}

/*************************************************/
/* rational functions */

/* num and den must be polynomials */
static BCValue rfrac_new(BCContext *ctx, BCValue num, BCValue den)
{
    BCType *t = NULL, *elem_type, *t1;
    BCValue v;
    BOOL use_gcd, is_int;
        
    if (cval_type(num) != CTYPE_POLY || cval_type(den) != CTYPE_POLY) {
        cval_type_error(ctx, "polynomial expected");
        goto fail;
    }

    /* convert to identical element type */
    elem_type = get_op2_type(ctx, num->type->elem_type,
                             den->type->elem_type, BC_OP2_ADD);
    if (!elem_type)
        goto fail;
    use_gcd = FALSE;
    is_int = FALSE;
    /* XXX: should convert rational polynomials to integer ones */
    if (elem_type->tag == CTYPE_INT) {
        is_int = TRUE;
        use_gcd = TRUE;
    } else if (elem_type->tag == CTYPE_FRAC ||
               (elem_type->tag == CTYPE_COMPLEX &&
                (elem_type->elem_type->tag == CTYPE_INT ||
                 elem_type->elem_type->tag == CTYPE_FRAC))) {
        use_gcd = TRUE;
    }

    t = ctype_new_free(ctx, CTYPE_POLY, elem_type);
    num = cval_convert(ctx, num, t);
    if (cval_is_error(num))
        goto fail;
    den = cval_convert(ctx, den, t);
    if (cval_is_error(den))
        goto fail;

    if (poly_deg(ctx, den) < 0) {
        cval_range_error(ctx, "division by zero");
        goto fail;
    }

    if (use_gcd) {
        BCValue g, r;
        
        g = poly_gcd(ctx, cval_dup(num), cval_dup(den));
        if (is_int) {
            int e;
            BCValue mult;
            BCPoly *pg;
            
            /* ensure that the result of the division by 'g' will be
               an integer polynomial */
            e = max_int(poly_deg(ctx, num), poly_deg(ctx, den)) -
                poly_deg(ctx, g) + 1;
            pg = &g->u.poly;
            mult = cval_pow(ctx, cval_dup(pg->tab[pg->len - 1]),
                            cint_from_int(ctx, e));
            num = cval_mul(ctx, num, cval_dup(mult));
            den = cval_mul(ctx, den, mult);
        }
        r = poly_divrem(ctx, &num, num, cval_dup(g));
        cval_free(ctx, r);
        r = poly_divrem(ctx, &den, den, g);
        cval_free(ctx, r);
    }

    t1 = ctype_new(ctx, CTYPE_RFRAC, t->elem_type);
    ctype_free(ctx, t);
    v = cval_new1(ctx, t1);
    ctype_free(ctx, t1);
    v->u.rfrac.num = num;
    v->u.rfrac.den = den;
    return v;
 fail:
    if (t)
        ctype_free(ctx, t);
    cval_free(ctx, num);
    cval_free(ctx, den);
    return BC_EXCEPTION;
}

static BCValue to_rfrac(BCContext *ctx, BCValue v1)
{
    BCValue den;
    if (cval_type(v1) == CTYPE_RFRAC)
        return v1;
    v1 = to_poly(ctx, v1);
    if (cval_is_error(v1))
        return v1;
    den = to_poly(ctx, cint_from_int(ctx, 1));
    return rfrac_new(ctx, v1, den);
}

static BCValue rfrac_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    BCValue v;
    v1 = to_rfrac(ctx, v1);
    if (cval_is_error(v1)) {
        cval_free(ctx, v2);
        return v1;
    }
    v2 = to_rfrac(ctx, v2);
    if (cval_is_error(v2)) {
        cval_free(ctx, v1);
        return v2;
    }
    switch(op) {
    case BC_OP2_ADD:
        v = rfrac_new(ctx, cval_add(ctx, cval_mul(ctx, cval_dup(v1->u.rfrac.num),
                                        cval_dup(v2->u.rfrac.den)), 
                               cval_mul(ctx, cval_dup(v2->u.rfrac.num),
                                        cval_dup(v1->u.rfrac.den))),
                      cval_mul(ctx, cval_dup(v1->u.rfrac.den),
                               cval_dup(v2->u.rfrac.den)));
        break;
    case BC_OP2_SUB:
        v = rfrac_new(ctx, cval_sub(ctx, cval_mul(ctx, cval_dup(v1->u.rfrac.num),
                                        cval_dup(v2->u.rfrac.den)), 
                                    cval_mul(ctx, cval_dup(v2->u.rfrac.num),
                                             cval_dup(v1->u.rfrac.den))),
                      cval_mul(ctx, cval_dup(v1->u.rfrac.den),
                               cval_dup(v2->u.rfrac.den)));
        break;
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        v = rfrac_new(ctx, cval_mul(ctx, cval_dup(v1->u.rfrac.num),
                                    cval_dup(v2->u.rfrac.num)), 
                      cval_mul(ctx, cval_dup(v1->u.rfrac.den),
                               cval_dup(v2->u.rfrac.den)));
        break;
    case BC_OP2_DIV:
    case BC_OP2_FRAC_DIV:
        v = rfrac_new(ctx, cval_mul(ctx, cval_dup(v1->u.rfrac.num),
                                    cval_dup(v2->u.rfrac.den)), 
                      cval_mul(ctx, cval_dup(v1->u.rfrac.den),
                               cval_dup(v2->u.rfrac.num)));
        break;
    default:
        v = cval_type_error(ctx, "unsupported operation");
        break;
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    return v;
}

static BCValue rfrac_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    switch(op) {
    case BC_OP1_NEG:
        return cval_mul(ctx, v1, cint_from_int(ctx, -1));
    default:
        cval_free(ctx, v1);
        return cval_type_error(ctx, "unsupported type");
    }
}

static void rfrac_toString(BCContext *ctx, DynBuf *d, BCValueConst v)
{
    dbuf_putc(d, '(');
    cval_toString(ctx, d, v->u.rfrac.num);
    dbuf_putstr(d, ")//(");
    cval_toString(ctx, d, v->u.rfrac.den);
    dbuf_putc(d, ')');
}

static BCValue rfrac_deriv(BCContext *ctx, BCValue v1)
{
    BCValue v, num, den;
    num = v1->u.rfrac.num;
    den = v1->u.rfrac.den;
    v = rfrac_new(ctx, cval_sub(ctx,
                                cval_mul(ctx, cval_deriv(ctx, cval_dup(num)),
                                         cval_dup(den)),
                                cval_mul(ctx, cval_dup(num),
                                         cval_deriv(ctx, cval_dup(den)))),
                  cval_mul(ctx, cval_dup(den), cval_dup(den)));
    cval_free(ctx, v1);
    return v;
}

static BCValue rfrac_apply(BCContext *ctx, BCValue v1, BCValue x)
{
    BCValue num, den;
    num = poly_apply(ctx, cval_dup(v1->u.rfrac.num), cval_dup(x));
    den = poly_apply(ctx, cval_dup(v1->u.rfrac.den), x);
    cval_free(ctx, v1);
    return cval_div(ctx, num, den);
}

/*************************************************/
/* power series */

/* Series are represented as X^emin*P(X) where P has a non zero
   constant term or P=0. */
/* len can be zero. A valid serie always has a non zero first term */
static BCValue ser_new(BCContext *ctx, const BCType *elem_type, int len,
                       int emin)
{
    BCType *t;
    BCValue v;
    BCPoly *p;
    int i;
    
    assert(len >= 0);
    if (!is_poly_elem_type(elem_type->tag))  {
        return cval_type_error(ctx, "only numeric types are allowed in series");
    }
    
    t = ctype_new(ctx, CTYPE_SER, elem_type);
    v = cval_new1(ctx, t);
    ctype_free(ctx, t);
    p = &v->u.poly;
    p->len = len;
    p->emin = emin;
    p->tab = malloc(sizeof(p->tab[0]) * len);
    for(i = 0; i < len; i++) {
        p->tab[i] = cval_convert(ctx, cint_from_int(ctx, 0), elem_type);
    }
    return v;
}

static BCValue ser_new2(BCContext *ctx, const BCType *t1, const BCType *t2,
                         int len, int emin)
{
    BCType *t;
    BCValue v;
    
    t = get_op2_type(ctx, t1, t2, BC_OP2_ADD);
    if (!t)
        return BC_EXCEPTION;
    v = ser_new(ctx, t, len, emin);
    ctype_free(ctx, t);
    return v;
}

/* return an empty serie matching the degree of the monome */
static BCValue ser_O(BCContext *ctx, BCValue v1)
{
    BCValue v;
    int n;
    if (cval_type(v1) <= CTYPE_POLY) {
        v1 = to_poly(ctx, v1);
        n = poly_deg(ctx, v1);
        if (n < 0)
            goto fail;
    } else if (cval_type(v1) == CTYPE_RFRAC) {
        n = poly_deg(ctx, v1->u.rfrac.num);
        if (n != 0) {
        fail:
            cval_free(ctx, v1);
            return cval_range_error(ctx, "invalid polynomial degree for O()");
        }
        n = poly_deg(ctx, v1->u.rfrac.den);
        assert(n >= 0);
        n = -n;
    } else {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "invalid type for O()");
    }
    v = ser_new(ctx, v1->type->elem_type, 0, n);
    cval_free(ctx, v1);
    return v;
}

/* v1 must be a polynomial or a series */
static int get_emin(BCContext *ctx, BCValueConst v1)
{
    const BCPoly *p = &v1->u.poly;
    int e;
    e = 0;
    while (e < p->len && cval_cmp_eq_int(ctx, p->tab[e], 0))
        e++;
    return e;
}

/* if 'a' is not a series, the returned series has n terms. */
static BCValue to_ser(BCContext *ctx, BCValue v1, int n)
{
    BCValue v;

    if (cval_type(v1) == CTYPE_SER) {
        return v1;
    } else if (cval_type(v1) <= CTYPE_POLY) {
        v1 = to_poly(ctx, v1);
        if (n <= 0) {
            /* XXX: should still use the polynomial degree ? */
            v = ser_new(ctx, v1->type->elem_type, 0, 0);
        } else {
            int e, i;
            BCPoly *p;
            e = get_emin(ctx, v1);
            v = ser_new(ctx, v1->type->elem_type, n, e);
            p = &v->u.poly;
            n = min_int(v1->u.poly.len - e, p->len);
            for(i = 0; i < n; i++) {
                set_value(ctx, &p->tab[i],
                          cval_dup(v1->u.poly.tab[i + e]));
            }
        }
    } else if (cval_type(v1) == CTYPE_RFRAC) {
        v = cval_div(ctx, cval_dup(v1->u.rfrac.num),
                     to_ser(ctx, cval_dup(v1->u.rfrac.den), n));
    } else {
        v = cval_type_error(ctx, "cannot convert to a series");
    }
    cval_free(ctx, v1);
    return v;
}

/* remove the trailing zero terms of the serie */
static void ser_trim(BCContext *ctx, BCValue v1)
{
    int i, j, new_len;
    BCPoly *p;
    p = &v1->u.poly;
    i = get_emin(ctx, v1);
    if (i <= 0)
        return;
    for(j = 0; j < i; j++)
        cval_free(ctx, p->tab[j]);
    new_len = p->len - i;
    for(j = 0; j < new_len; j++)
        p->tab[j] = p->tab[j + i];
    p->emin += i;
    p->tab = realloc(p->tab, sizeof(p->tab[0]) * new_len);
    p->len = new_len;
}

static BCValue ser_neg(BCContext *ctx, BCValue v1)
{
    BCValue r;
    int i;
    BCPoly *p1, *p;
    
    p1 = &v1->u.poly;
    r = ser_new(ctx, v1->type->elem_type, p1->len, p1->emin);
    p = &r->u.poly;
    for(i = 0; i < p1->len; i++) {
        set_value(ctx, &p->tab[i], cval_neg(ctx, cval_dup(p1->tab[i])));
    }
    cval_free(ctx, v1);
    return r;
}

static BCValue ser_convert(BCContext *ctx, BCValue v1, const BCType *elem_type)
{
    BCValue v, e1;
    BCPoly *p, *p1;
    int i;

    p1 = &v1->u.poly;
    v = ser_new(ctx, elem_type, p1->len, p1->emin);
    if (cval_is_error(v))
        goto done;
    p = &v->u.poly;
    for(i = 0; i < p1->len; i++) {
        e1 = cval_convert(ctx, cval_dup(p1->tab[i]), elem_type);
        if (cval_is_error(e1)) {
            cval_free(ctx, v);
            v = BC_EXCEPTION;
            goto done;
        }
        set_value(ctx, &p->tab[i], e1);
    }
    ser_trim(ctx, v);
 done:
    cval_free(ctx, v1);
    return v;
}

/* at least one argument must be a serie */
static BCValue ser_add(BCContext *ctx, BCValue v1, BCValue v2)
{
    int n, emin, i, j, d;
    BCValue r;
    BCPoly *p1, *p2, *p;
    
    if (cval_type(v1) != CTYPE_SER) {
        BCValue tmp = v1;
        v1 = v2;
        v2 = tmp;
    }
    p1 = &v1->u.poly;
    d = p1->emin + p1->len;
    /* v2 is the possible non serie argument */
    if (cval_type(v2) <= CTYPE_POLY) {
        v2 = to_poly(ctx, v2);
        if (d <= 0) {
            cval_free(ctx, v2);
            return v1;
        }
        /* emin = 0 for a polynomial */
    } else if (cval_type(v2) == CTYPE_RFRAC) {
        /* compute the emin of the rational fonction */
        i = get_emin(ctx, v2->u.rfrac.num) - get_emin(ctx, v2->u.rfrac.den);
        if (d <= i) {
            cval_free(ctx, v2);
            return v1;
        }
        /* compute the serie with the required terms */
        v2 = to_ser(ctx, v2, d - i);
    } else {
        d = min_int(d, v2->u.poly.emin + v2->u.poly.len);
    }

    p2 = &v2->u.poly;
    emin = min_int(p1->emin, p2->emin);
    n = d - emin;
    r = ser_new2(ctx, v1->type->elem_type, v2->type->elem_type, n, emin);
    p = &r->u.poly;
    for(i = 0; i < n; i++) {
        BCValue c1, c2;
        j = i + emin - p1->emin;
        if (j >= 0 && j < p1->len)
            c1 = cval_dup(p1->tab[j]);
        else
            c1 = cval_convert(ctx, cint_from_int(ctx, 0), r->type->elem_type);
        j = i + emin - p2->emin;
        if (j >= 0 && j < p2->len)
            c2 = cval_dup(p2->tab[j]);
        else
            c2 = cval_convert(ctx, cint_from_int(ctx, 0), r->type->elem_type);
        set_value(ctx, &p->tab[i], cval_add(ctx, c1, c2));
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    ser_trim(ctx, r);
    return r;
}

static BCValue ser_sub(BCContext *ctx, BCValue v1, BCValue v2)
{
    return ser_add(ctx, v1, cval_neg(ctx, v2));
}

/* at least one argument must be a serie */
static BCValue ser_mul(BCContext *ctx, BCValue v1, BCValue v2)
{
    int n, i, j, emin;
    BCValue r;
    BCPoly *p1, *p2, *p;
    
    if (cval_type(v1) != CTYPE_SER)
        v1 = to_ser(ctx, v1, v2->u.poly.len);
    if (cval_type(v2) != CTYPE_SER)
        v2 = to_ser(ctx, v2, v1->u.poly.len);
    p1 = &v1->u.poly;
    p2 = &v2->u.poly;
    emin = p1->emin + p2->emin;
    n = min_int(p1->len, p2->len);
    r = ser_new2(ctx, v1->type->elem_type, v2->type->elem_type, n, emin);
    p = &r->u.poly;
    for(i = 0; i < n; i++) {
        for(j = 0; j < n - i; j++) {
            set_value(ctx, &p->tab[i + j],
                      cval_add(ctx, cval_dup(p->tab[i + j]), 
                               cval_mul(ctx, cval_dup(p1->tab[i]),
                                        cval_dup(p2->tab[j]))));
        }
    }
    cval_free(ctx, v1);
    cval_free(ctx, v2);
    ser_trim(ctx, r);
    return r;
}

static BCValue ser_inverse(BCContext *ctx, BCValue v1, BOOL is_frac)
{
    BCValue r, sum, c;
    int n, i, j;
    BCPoly *p1, *p;
    BCType *elem_type;

    p1 = &v1->u.poly;
    n = p1->len;
    if (n == 0) {
        cval_free(ctx, v1);
        return cval_range_error(ctx, "division by zero");
    }
    elem_type = get_inverse2_type(ctx, v1->type->elem_type, is_frac);
    r = ser_new(ctx, elem_type, n, -p1->emin);
    ctype_free(ctx, elem_type);
    p = &r->u.poly;
    if (is_frac)
        c = cval_frac_div(ctx, cint_from_int(ctx, 1), cval_dup(p1->tab[0]));
    else
        c = cval_inverse(ctx, cval_dup(p1->tab[0]));
    set_value(ctx, &p->tab[0], c);
    for(i = 1; i < n; i++) {
        sum = cint_from_int(ctx, 0);
        for(j = 1; j <= i; j++) {
            sum = cval_add(ctx, sum, cval_mul(ctx, cval_dup(p1->tab[j]),
                                              cval_dup(p->tab[i - j])));
        }
        sum = cval_neg(ctx, cval_mul(ctx, sum, cval_dup(p->tab[0])));
        set_value(ctx, &p->tab[i], sum);
    }
    cval_free(ctx, v1);
    ser_trim(ctx, r); /* no need to trim except if rounding error */
    return r;
}

/* at least one argument must be a serie */
static BCValue ser_div(BCContext *ctx, BCValue v1, BCValue v2, BOOL is_frac)
{
    if (cval_type(v1) != CTYPE_SER)
        v1 = to_ser(ctx, v1, v2->u.poly.len);
    if (cval_type(v2) != CTYPE_SER)
        v2 = to_ser(ctx, v2, v1->u.poly.len);
    is_frac = is_frac || is_complex_frac(v1->type->elem_type);
    return ser_mul(ctx, v1, ser_inverse(ctx, v2, is_frac));
}

static BCValue ser_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    switch(op) {
    case BC_OP2_ADD:
        return ser_add(ctx, v1, v2);
    case BC_OP2_SUB:
        return ser_sub(ctx, v1, v2);
    case BC_OP2_MUL:
    case BC_OP2_DOT_MUL:
        return ser_mul(ctx, v1, v2);
    case BC_OP2_DIV:
    case BC_OP2_FRAC_DIV:
        return ser_div(ctx, v1, v2, (op == BC_OP2_FRAC_DIV));
    default:
        cval_free(ctx, v1);
        cval_free(ctx, v2);
        return cval_type_error(ctx, "unsupported operation");
    }
}

static BCValue ser_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue v, val;
    BCPoly *p;
    int i;

    if (n_args != 2) {
        val = cval_type_error(ctx, "series have a single dimension");
        goto done;
    }
    v = args[0];
    p = &v->u.poly;
    if (cval_type(args[1]) == CTYPE_RANGE) {
        val = cval_type_error(ctx, "ranges are not supported for series");
        goto done;
    } else {
        int idx;
        if (cint_to_int(ctx, &idx, args[1])) {
            val = BC_EXCEPTION;
            goto done;
        }
        idx -= p->emin;
        if (idx < 0 || idx >= p->len) {
            val = cval_convert(ctx, cint_from_int(ctx, 0), v->type->elem_type);
        } else {
            val = cval_dup(p->tab[idx]);
        }
    }
 done:
    for(i = 0; i < n_args; i++)
        cval_free(ctx, args[i]);
    return val;
}

static void ser_toString(BCContext *ctx, DynBuf *d, BCValueConst  v1)
{
    int i, pos;
    BCValue c;
    const BCPoly *p;
    BOOL is_first;

    is_first = TRUE;
    p = &v1->u.poly;
    for(i = 0; i < p->len; i++) {
        c = p->tab[i];
        if (!cval_cmp_eq_int(ctx, c, 0)) {
            pos = d->size;
            monomial_toString(ctx, d, c, i + p->emin);
            if (d->buf[pos] != '-' && !is_first) {
                insert_plus(d, pos);
            }
            is_first = FALSE;
        }
    }

    if (!is_first)
        dbuf_putc(d, '+');
    
    i = p->len + p->emin;
    dbuf_putstr(d, "O(");
    if (i == 0) {
        dbuf_putstr(d, "1");
    } else {
        dbuf_putstr(d, "X");
        if (i != 1) {
            dbuf_printf(d, "^%d", i);
        }
    }
    dbuf_putstr(d, ")");
}

static BCValue ser_apply(BCContext *ctx, BCValue v1, BCValue x)
{
    BCValue v;
    int emin;
    emin = v1->u.poly.emin;
    v = poly_apply(ctx, v1, cval_dup(x));
    if (cval_is_error(v))
        goto done;
    if (emin != 0) {
        v = cval_mul(ctx, v, cval_pow(ctx, cval_dup(x), cint_from_int(ctx, emin)));
    }
 done:
    cval_free(ctx, x);
    return v;
}

static BCValue ser_deriv(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCPoly *p1, *p;
    int i, j, n;
    
    p1 = &v1->u.poly;
    if (p1->len == 0 && p1->emin == 0) {
        v = ser_new(ctx, v1->type->elem_type, 0, 0);
    } else {
        n = p1->len;
        v = ser_new(ctx, v1->type->elem_type, n, p1->emin - 1);
        p = &v->u.poly;
        for(i = 0; i < n; i++) {
            j = p1->emin + i;
            set_value(ctx, &p->tab[i],
                      cval_mul(ctx, cval_dup(p1->tab[i]), cint_from_int(ctx, j)));
        }
    }
    cval_free(ctx, v1);
    ser_trim(ctx, v);
    return v;
}

static BCValue ser_integ(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCPoly *p1, *p;
    BCType *elem_type;
    int i, j, n;
    
    p1 = &v1->u.poly;
    n = p1->len;
    elem_type = get_inverse_type(ctx, v1->type->elem_type);
    v = ser_new(ctx, elem_type, n, p1->emin + 1);
    ctype_free(ctx, elem_type);
    p = &v->u.poly;
    for(i = 0; i < n; i++) {
        j = p1->emin + i;
        if (j == -1) {
            if (!cval_cmp_eq_int(ctx, p1->tab[i], 0)) {
                cval_free(ctx, v);
                cval_free(ctx, v1);
                return cval_range_error(ctx, "cannot represent integ(1/X)");
            }
        } else {
            set_value(ctx, &p->tab[i],
                      cval_div(ctx, cval_dup(p1->tab[i]), cint_from_int(ctx, j + 1)));
        }
    }
    cval_free(ctx, v1);
    ser_trim(ctx, v);
    return v;
}

/* remove the first term */
static BCValue ser0(BCContext *ctx, BCValue v1)
{
    BCValue v;
    BCPoly *p1, *p;
    int i;
    p1 = &v1->u.poly;
    v = ser_new(ctx, v1->type->elem_type, p1->len - 1, p1->emin + 1);
    p = &v->u.poly;
    for(i = 0; i < p1->len - 1; i++) {
        set_value(ctx, &p->tab[i], cval_dup(p1->tab[i + 1]));
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue ser_exp(BCContext *ctx, BCValue v1)
{
    BCValue r, c;
    BCPoly *p1, *p;
    BCType *elem_type;
    int n, i;
    
    p1 = &v1->u.poly;
    if (p1->emin < 0) {
        cval_free(ctx, v1);
        return cval_range_error(ctx, "negative exponent in exp");
    }
    n = p1->emin + p1->len;
    if (p1->emin > 0) {
        /* convert so that fractions stay as fractions */
        c = cval_convert(ctx, cint_from_int(ctx, 1), v1->type->elem_type);
    } else {
        if (is_complex_frac(v1->type->elem_type)) {
            cval_free(ctx, v1);
            return cval_range_error(ctx, "non zero exponent in exp with rational type");
        }
        c = cval_exp(ctx, cval_dup(p1->tab[0]));
        v1 = ser0(ctx, v1);
    }
    p1 = &v1->u.poly;
    elem_type = get_inverse_type(ctx, v1->type->elem_type);
    r = ser_new(ctx, elem_type, n, 0);
    ctype_free(ctx, elem_type);
    p = &r->u.poly;
    for(i = 0; i < n; i++) {
        set_value(ctx, &p->tab[i],
                  cval_div(ctx, cval_dup(c), cval_fact1(ctx, i)));
    }
    cval_free(ctx, c);
    return ser_apply(ctx, r, v1);
}

static BCValue ser_log(BCContext *ctx, BCValue v1)
{
    BCValue r;
    if (v1->u.poly.emin != 0) {
        cval_free(ctx, v1);
        return cval_range_error(ctx, "log argument must have a non zero constant term");
    }
    r = cval_integ(ctx, cval_div(ctx, cval_deriv(ctx, cval_dup(v1)), cval_dup(v1)));
    /* add the missing constant */
    if (!cval_cmp_eq_int(ctx, v1->u.poly.tab[0], 1)) {
        if (is_complex_frac(v1->type->elem_type)) {
            cval_free(ctx, r);
            cval_free(ctx, v1);
            return cval_range_error(ctx, "non unit argument in log with rational type");
        }
        r = cval_add(ctx, r, cval_log(ctx, cval_dup(v1->u.poly.tab[0])));
    }
    cval_free(ctx, v1);
    return r;
}

static BCValue ser_re_im(BCContext *ctx, BCValue v1, BOOL is_im)
{
    BCValue v, e1;
    BCPoly *p1, *p;
    int i;
    p1 = &v1->u.poly;
    v = ser_new(ctx, v1->type->elem_type->elem_type, p1->len, p1->emin);
    p = &v->u.poly;
    for(i = 0; i < p1->len; i++) {
        e1 = p1->tab[i];
        assert(cval_type(e1) == CTYPE_COMPLEX);
        if (is_im)
            e1 = cval_dup(e1->u.complex.im);
        else
            e1 = cval_dup(e1->u.complex.re);
        set_value(ctx, &p->tab[i], e1);
    }
    cval_free(ctx, v1);
    return v;
}

static BCValue ser_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    BCValue v;
    switch(op) {
    case BC_OP1_NEG:
        return ser_neg(ctx, v1);
    case BC_OP1_LOG:
        return ser_log(ctx, v1);
    case BC_OP1_EXP:
        return ser_exp(ctx, v1);
        /* XXX: could ensure that real series get real result */
    case BC_OP1_SIN:
        {
            BCValue t;
            t = cval_exp(ctx, cval_mul(ctx, cval_dup(v1), complex_new_int(ctx, 0, 1)));
            if (cval_is_error(t)) {
                v = t;
                break;
            }
            if (is_real_number(v1->type->elem_type)) {
                v = ser_re_im(ctx, t, TRUE);
            } else {
                v = cval_div(ctx, cval_sub(ctx, cval_dup(t), cval_inverse(ctx, cval_dup(t))),
                             complex_new_int(ctx, 0, 2));
                cval_free(ctx, t);
            }
        }
        break;
    case BC_OP1_COS:
        {
            BCValue t;
            t = cval_exp(ctx, cval_mul(ctx, cval_dup(v1), complex_new_int(ctx, 0, 1)));
            if (cval_is_error(t)) {
                v = t;
                break;
            }
            if (is_real_number(v1->type->elem_type)) {
                v = ser_re_im(ctx, t, FALSE);
            } else {
                v = cval_div(ctx, cval_add(ctx, cval_dup(t), cval_inverse(ctx, cval_dup(t))),
                             cint_from_int(ctx, 2));
                cval_free(ctx, t);
            }
        }
        break;
    case BC_OP1_TAN:
        v = cval_div(ctx, cval_sin(ctx, cval_dup(v1)), cval_cos(ctx, cval_dup(v1)));
        break;
    default:
        v = cval_type_error(ctx, "unsupported type");
    }
    cval_free(ctx, v1);
    return v;
}

/*************************************************/
/* generic value */

static void cval_typeof1(BCContext *ctx, DynBuf *d, const BCType *t)
{
    dbuf_putstr(d, ctype_str[t->tag]);
    if (t->elem_type) {
        dbuf_putc(d, '(');
        cval_typeof1(ctx, d, t->elem_type);
        dbuf_putc(d, ')');
    }
}

static BCValue cval_typeof(BCContext *ctx, BCValue v1)
{
    BCValue r;
    DynBuf dbuf;
    dbuf_init(&dbuf);
    cval_typeof1(ctx, &dbuf, v1->type);
    r = cstring_new(ctx, (char *)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    cval_free(ctx, v1);
    return r;
}

static BCValue cval_convert(BCContext *ctx, BCValue v1, const BCType *t1)
{
    BCValue v;

    if (same_type(ctx, v1->type, t1))
        return v1;
    switch(t1->tag) {
    case CTYPE_INT:
        v = to_cint(ctx, v1);
        break;
    case CTYPE_FRAC:
        v = to_cfrac(ctx, v1);
        break;
    case CTYPE_FLOAT:
        v = to_float1(ctx, v1, TRUE);
        break;
    case CTYPE_DECIMAL:
        v = to_dec1(ctx, v1, TRUE);
        break;
    case CTYPE_COMPLEX:
        v1 = to_complex(ctx, v1);
        if (cval_is_error(v1))
            goto fail;
        v = complex_new2(ctx, cval_dup(v1->u.complex.re),
                         cval_dup(v1->u.complex.im),
                         t1->elem_type);
        cval_free(ctx, v1);
        break;
    case CTYPE_POLY:
        v1 = to_poly(ctx, v1);
        if (cval_is_error(v1))
            goto fail;
        v = poly_convert(ctx, v1, t1->elem_type);
        break;
    case CTYPE_RFRAC:
        v1 = to_rfrac(ctx, v1);
        if (cval_is_error(v1))
            goto fail;
        v = rfrac_new(ctx, poly_convert(ctx, cval_dup(v1->u.rfrac.num), t1->elem_type),
                      poly_convert(ctx, cval_dup(v1->u.rfrac.den), t1->elem_type));
        cval_free(ctx, v1);
        break;
    case CTYPE_TENSOR:
        if (cval_type(v1) != CTYPE_TENSOR)
            goto fail;
        v = tensor_convert(ctx, v1, t1->elem_type);
        cval_free(ctx, v1);
        break;
    case CTYPE_SER:
        if (cval_type(v1) != CTYPE_SER)
            goto fail;
        v = ser_convert(ctx, v1, t1->elem_type);
        break;
    default:
    fail:
        cval_free(ctx, v1);
        v = cval_type_error(ctx, "cannot convert type");
    }
    return v;
}

static BCValue cval_op2(BCContext *ctx, BCValue v1, BCValue v2, BCOP2Enum op)
{
    int max_type = max_int(cval_type(v1), cval_type(v2));

    if (max_type == CTYPE_INT || max_type == CTYPE_BOOL) {
        if (op == BC_OP2_DIV) {
            return cdec_op2(ctx, v1, v2, op);
        } else {
            return cint_op2(ctx, v1, v2, op);
        }
    } else if (max_type == CTYPE_FRAC) {
        return cfrac_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_DECIMAL) {
        return cdec_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_FLOAT) {
        return cfloat_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_COMPLEX) {
        return complex_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_TENSOR) {
        return tensor_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_POLY) {
        return poly_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_RFRAC) {
        return rfrac_op2(ctx, v1, v2, op);
    } else if (max_type == CTYPE_SER) {
        return ser_op2(ctx, v1, v2, op);
    } else if (cval_type(v1) == CTYPE_STRING &&
               cval_type(v2) == CTYPE_STRING &&
               op == BC_OP2_ADD) {
        return cstring_concat(ctx, v1, v2);
    } else
    {
        return cval_type_error(ctx, "incompatible types");
    }
}

static BCValue cval_add(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_ADD);
}

static BCValue cval_sub(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_SUB);
}

static BCValue cval_mul(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_MUL);
}

static BCValue cval_dot_mul(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_DOT_MUL);
}

static BCValue cval_div(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_DIV);
}

static BCValue cval_mod(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_MOD);
}

static BCValue cval_or(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_OR);
}

static BCValue cval_and(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_AND);
}

static BCValue cval_xor(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_XOR);
}

static BCValue cval_cmp_eq(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_CMP_EQ);
}

static BCValue cval_cmp_neq(BCContext *ctx, BCValue v1, BCValue v2)
{
    BCValue v;
    int res;
    v = cval_op2(ctx, v1, v2, BC_OP2_CMP_EQ);
    if (cval_is_error(v))
        return v;
    res = cbool_to_int(v);
    cval_free(ctx, v);
    return cbool_new(ctx, !res);
}

static BCValue cval_cmp_lt(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_CMP_LT);
}

static BCValue cval_cmp_le(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_CMP_LE);
}

static BOOL cval_cmp2(BCContext *ctx, BCValueConst v1, BCValueConst v2, BCOP2Enum op)
{
    BCValue r;
    BOOL res;

    r = cval_op2(ctx, cval_dup(v1), cval_dup(v2), op);
    res = cbool_to_int(r);
    cval_free(ctx, r);
    return res;
}

static BOOL cval_cmp_eq2(BCContext *ctx, BCValueConst v1, BCValueConst v2)
{
    return cval_cmp2(ctx, v1, v2, BC_OP2_CMP_EQ);
}

static BOOL cval_cmp_lt2(BCContext *ctx, BCValueConst v1, BCValueConst v2)
{
    return cval_cmp2(ctx, v1, v2, BC_OP2_CMP_LT);
}

static BOOL cval_cmp_le2(BCContext *ctx, BCValueConst v1, BCValueConst v2)
{
    return cval_cmp2(ctx, v1, v2, BC_OP2_CMP_LE);
}

static BOOL cval_cmp_eq_int(BCContext *ctx, BCValueConst a, int b)
{
    int res;
    BCValue v = cval_op2(ctx, cval_dup(a), cint_from_int(ctx, b),
                         BC_OP2_CMP_EQ);
    if (cval_is_error(v))
        return FALSE;
    res = cbool_to_int(v);
    cval_free(ctx, v);
    return res;
}

static BOOL cval_cmp_lt_int(BCContext *ctx, BCValueConst a, int b)
{
    int res;
    BCValue v = cval_op2(ctx, cval_dup(a), cint_from_int(ctx, b),
                         BC_OP2_CMP_LT);
    if (cval_is_error(v))
        return FALSE;
    res = cbool_to_int(v);
    cval_free(ctx, v);
    return res;
}

/* the string is appended to dbuf. 'v1' is not freed */
static void cval_toString(BCContext *ctx, DynBuf *d, BCValueConst v1)
{
    if (cval_is_error(v1)) {
        dbuf_putstr(d, "[exception]");
    } else if (cval_type(v1) == CTYPE_INT) {
        if (ctx->hex_output) {
            cint_to_string(ctx, d, v1, 16);
        } else {
            cint_to_string(ctx, d, v1, 10);
        }
    } else if (cval_type(v1) == CTYPE_FRAC) {
        cfrac_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_DECIMAL) {
        cdec_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_FLOAT) {
        if (ctx->hex_output) {
            cfloat_toString(ctx, d, v1, 16);
        } else {
            cfloat_toString(ctx, d, v1, 10);
        }
    } else if (cval_type(v1) == CTYPE_COMPLEX) {
        complex_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_FUNCTION) {
        dbuf_printf(d, "[function %s]", v1->u.function.name);
    } else if (cval_type(v1) == CTYPE_NULL) {
        dbuf_putstr(d, "null");
    } else if (cval_type(v1) == CTYPE_BOOL) {
        dbuf_putstr(d, v1->u.bool_val ? "true" : "false");
    } else if (cval_type(v1) == CTYPE_ARRAY) {
        carray_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_STRING) {
        cstring_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_TENSOR) {
        tensor_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_POLY) {
        poly_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_RFRAC) {
        rfrac_toString(ctx, d, v1);
    } else if (cval_type(v1) == CTYPE_SER) {
        ser_toString(ctx, d, v1);
    } else
    {
        dbuf_putstr(d, "[cannot display this object]");
    }
}

static __maybe_unused void cval_dump(BCContext *ctx, const char *str, BCValueConst val)
{
    DynBuf dbuf;
    dbuf_init(&dbuf);
    cval_toString(ctx, &dbuf, val);
    dbuf_putc(&dbuf, '\0');
    printf("%s=%s\n", str, (char *)dbuf.buf);
    dbuf_free(&dbuf);
}

static BCValue cval_inverse(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) <= CTYPE_FLOAT ||
        cval_type(v1) == CTYPE_POLY || 
        cval_type(v1) == CTYPE_RFRAC) {
        return cval_div(ctx, cint_from_int(ctx, 1), v1);
    } else if (cval_type(v1) == CTYPE_COMPLEX) {
        return complex_inverse(ctx, v1);
    } else if (cval_type(v1) == CTYPE_TENSOR) {
        return matrix_inverse(ctx, v1);
    } else if (cval_type(v1) == CTYPE_SER) {
        return ser_inverse(ctx, v1, FALSE);
    } else 
    {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "incompatible type");
    }
}

static BCValue generic_pow(BCContext *ctx, BCValue a, BCValue b)
{
    BCValue r;
    int s;
    
    if (cval_type(a) == CTYPE_TENSOR) {
        int n = matrix_check_square(ctx, a);
        if (n < 0) {
            r = BC_EXCEPTION;
            goto done;
        }
        r = matrix_idn(ctx, cint_from_int(ctx, n));
    } else if (cval_type(a) == CTYPE_SER) {
        r = to_ser(ctx, cint_from_int(ctx, 1), a->u.poly.len);
    } else {
        r = cval_convert(ctx, cint_from_int(ctx, 1), a->type);
    }
    
    if (!cval_cmp_eq_int(ctx, b, 0)) {
        if (cval_cmp_lt_int(ctx, b, 0)) {
            a = cval_inverse(ctx, a);
            if (!a) {
                r = BC_EXCEPTION;
                goto done;
            }
            b = cval_neg(ctx, b);
        }
        /* XXX: use more efficient algo with shr */
        for(;;) {
            bf_get_int32(&s, &b->u.cint, BF_GET_INT_MOD);
            if (s & 1) {
                r = cval_mul(ctx, r, cval_dup(a));
            }
            b = cint_shl(ctx, b, cint_from_int(ctx, -1));
            if (cval_cmp_eq_int(ctx, b, 0))
                break;
            a = cval_mul(ctx, cval_dup(a), a);
        }
    }
 done:
    cval_free(ctx, a);
    cval_free(ctx, b);
    return r;
}

static BCValue cval_pow(BCContext *ctx, BCValue v1, BCValue v2)
{
    int max_type = max_int(cval_type(v1), cval_type(v2));
    if (max_type == CTYPE_INT) {
        if (cval_cmp_lt_int(ctx, v2, 0)) {
            /* for convenience, return a float */
            return cdec_op2(ctx, v1, v2, BC_OP2_POW);
        } else {
            return cint_op2(ctx, v1, v2, BC_OP2_POW);
        }
    } else if (max_type == CTYPE_DECIMAL) {
        return cdec_op2(ctx, v1, v2, BC_OP2_POW);
    } else if (max_type == CTYPE_FLOAT) {
        return cfloat_op2(ctx, v1, v2, BC_OP2_POW);
    } else if (cval_type(v1) == CTYPE_TENSOR &&
               cval_type(v2) == CTYPE_TENSOR) {
        return tensor_op2(ctx, v1, v2, BC_OP2_POW);
    } else if (cval_type(v2) == CTYPE_INT) {
        /* for convenience, we do not systematically handle complex ^
           int as floating point numbers */
        return generic_pow(ctx, v1, v2);
    } else if (max_type == CTYPE_COMPLEX) {
        return complex_op2(ctx, v1, v2, BC_OP2_POW);
    } else if (cval_type(v1) == CTYPE_SER) {
        return ser_exp(ctx, cval_mul(ctx, ser_log(ctx, v1), v2));
    } else
    {
        return cval_type_error(ctx, "incompatible types");
    }
}

static BCValue cval_divrem(BCContext *ctx, BCValue v1, BCValue v2)
{
    return cval_op2(ctx, v1, v2, BC_OP2_DIVREM);
}

/* only for integer or complex(int) */
static BCValue cval_divexact(BCContext *ctx,  BCValue v1, BCValue v2)
{
    BCValue arr, q, r;
    BOOL is_zero;
    
    /* XXX: add a div exact operator ? */
    arr = cval_divrem(ctx, v1, v2);
    if (cval_is_error(arr))
        return arr;
    q = cval_dup(arr->u.array.tab[0]);
    r = cval_dup(arr->u.array.tab[1]);
    cval_free(ctx, arr);
    is_zero = cval_cmp_eq_int(ctx, r, 0);
    cval_free(ctx, r);
    if (!is_zero) {
        cval_free(ctx, q);
        return cval_range_error(ctx, "division is not exact");
    }
    return q;
}

static BCValue cval_gcd(BCContext *ctx, BCValue v1, BCValue v2)
{
    if (cval_type(v1) == CTYPE_INT && cval_type(v1) == CTYPE_INT) {
        return cint_gcd(ctx, v1, v2);
    } else if (cval_type(v1) == CTYPE_POLY && cval_type(v1) == CTYPE_POLY) {
        return poly_gcd(ctx, v1, v2);
    } else
    {
        return cval_type_error(ctx, "incompatible types");
    }
}

static BCValue cval_op1(BCContext *ctx, BCValue v1, BCOP1Enum op)
{
    if (op == BC_OP1_SQRT ||
        op == BC_OP1_EXP ||
        op == BC_OP1_LOG ||
        op == BC_OP1_SIN ||
        op == BC_OP1_COS ||
        op == BC_OP1_TAN ||
        op == BC_OP1_ASIN ||
        op == BC_OP1_ACOS ||
        op == BC_OP1_ATAN) {
        if (cval_type(v1) < CTYPE_DECIMAL) {
            v1 = to_dec(ctx, v1);
            if (cval_is_error(v1))
                return v1;
        }
    } else if ((op == BC_OP1_CONJ || op == BC_OP1_RE || op == BC_OP1_IM) &&
               cval_type(v1) <= CTYPE_FLOAT) {
        if (op == BC_OP1_IM) {
            BCValue v;
            v = cval_convert(ctx, cint_from_int(ctx, 0), v1->type);
            cval_free(ctx, v1);
            return v;
        } else {
            return v1;
        }
    }
    switch(cval_type(v1)) {
    case CTYPE_INT:
        return cint_op1(ctx, v1, op);
    case CTYPE_FRAC:
        return cfrac_op1(ctx, v1, op);
    case CTYPE_DECIMAL:
        return cdec_op1(ctx, v1, op);
    case CTYPE_FLOAT:
        return cfloat_op1(ctx, v1, op);
    case CTYPE_COMPLEX:
        return complex_op1(ctx, v1, op);
    case CTYPE_POLY:
        return poly_op1(ctx, v1, op);
    case CTYPE_RFRAC:
        return rfrac_op1(ctx, v1, op);
    case CTYPE_SER:
        return ser_op1(ctx, v1, op);
    case CTYPE_TENSOR:
        return tensor_op1(ctx, v1, op);
    default:
        return cval_type_error(ctx, "unsupported type");
    }
}
    
static BCValue cval_neg(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_NEG);
}

static BCValue cval_abs(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_ABS);
}

static BCValue cval_trunc(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_TRUNC);
}

static BCValue cval_floor(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_FLOOR);
}

static BCValue cval_ceil(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_CEIL);
}

static BCValue cval_round(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_ROUND);
}

static BCValue cval_conj(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_CONJ);
}

static BCValue cval_re(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_RE);
}

static BCValue cval_im(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_IM);
}

static BCValue cval_sqrt(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) <= CTYPE_FLOAT &&
        cval_cmp_lt_int(ctx, v1, 0)) {
        /* for convenience, return a complex value */
        return complex_new(ctx, cint_from_int(ctx, 0),
                           cval_sqrt(ctx, cval_neg(ctx, v1)));
    } else {
        return cval_op1(ctx, v1, BC_OP1_SQRT);
    }
}

static BCValue cval_exp(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_EXP);
}

static BCValue cval_log(BCContext *ctx, BCValue v1)
{
    /* for convenience in the scalar log case, we
       we redirect to complex in case the argument is negative */
    if (cval_type(v1) <= CTYPE_FLOAT &&
        cval_cmp_lt_int(ctx, v1, 0)) {
        v1 = to_complex(ctx, v1);
    }
    return cval_op1(ctx, v1, BC_OP1_LOG);
}

static BCValue cval_sin(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_SIN);
}

static BCValue cval_cos(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_COS);
}

static BCValue cval_tan(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_TAN);
}

static BCValue cval_asin(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_ASIN);
}

static BCValue cval_acos(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_ACOS);
}

static BCValue cval_atan(BCContext *ctx, BCValue v1)
{
    return cval_op1(ctx, v1, BC_OP1_ATAN);
}

static BCValue cval_atan2(BCContext *ctx, BCValue v1, BCValue v2)
{
    int max_type = max_int(cval_type(v1), cval_type(v2));
    if (max_type <= CTYPE_DECIMAL) {
        return cdec_op2(ctx, v1, v2, BC_OP2_ATAN2);
    } else if (max_type == CTYPE_FLOAT) {
        return cfloat_op2(ctx, v1, v2, BC_OP2_ATAN2);
    } else {
        return cval_type_error(ctx, "incompatible types");
    }
}

static BCValue cval_log2(BCContext *ctx, BCValue v1)
{
    return cval_mul(ctx, cval_log(ctx, v1), cdec_const(ctx, BC_CONST_LOG2E));
}

static BCValue cval_log10(BCContext *ctx, BCValue v1)
{
    return cval_mul(ctx, cval_log(ctx, v1), cdec_const(ctx, BC_CONST_LOG10E));
}

static BCValue cval_sinh(BCContext *ctx, BCValue v1)
{
    BCValue e, r;
    e = cval_exp(ctx, v1);
    r = cval_div(ctx, cval_sub(ctx, cval_dup(e), cval_inverse(ctx, cval_dup(e))),
                 cint_from_int(ctx, 2));
    cval_free(ctx, e);
    return r;
}

static BCValue cval_cosh(BCContext *ctx, BCValue v1)
{
    BCValue e, r;
    e = cval_exp(ctx, v1);
    r = cval_div(ctx, cval_add(ctx, cval_dup(e), cval_inverse(ctx, cval_dup(e))),
                 cint_from_int(ctx, 2));
    cval_free(ctx, e);
    return r;
}

static BCValue cval_tanh(BCContext *ctx, BCValue v1)
{
    BCValue e, r;
    e = cval_exp(ctx, cval_mul(ctx, cint_from_int(ctx, 2), v1));
    r = cval_div(ctx, cval_sub(ctx, cval_dup(e), cint_from_int(ctx, 1)),
                 cval_add(ctx, cval_dup(e), cint_from_int(ctx, 1)));
    cval_free(ctx, e);
    return r;
}

static BCValue cval_asinh(BCContext *ctx, BCValue x)
{
    BCValue r;
    r = cval_sqrt(ctx, cval_add(ctx, cval_mul(ctx, cval_dup(x), cval_dup(x)),
                                cint_from_int(ctx, 1)));
    return cval_log(ctx, cval_add(ctx, r, x));
}

static BCValue cval_acosh(BCContext *ctx, BCValue x)
{
    BCValue r;
    r = cval_sqrt(ctx, cval_sub(ctx, cval_mul(ctx, cval_dup(x), cval_dup(x)),
                                cint_from_int(ctx, 1)));
    return cval_log(ctx, cval_add(ctx, r, x));
}

static BCValue cval_atanh(BCContext *ctx, BCValue x)
{
    BCValue r;
    r = cval_div(ctx, cval_add(ctx, cint_from_int(ctx, 1), cval_dup(x)),
                 cval_sub(ctx, cint_from_int(ctx, 1), cval_dup(x)));
    cval_free(ctx, x);
    return cval_div(ctx, cval_log(ctx, r), cint_from_int(ctx, 2));
}

static BCValue cval_todb(BCContext *ctx, BCValue v1)
{
    return cval_mul(ctx, cval_log10(ctx, v1), cint_from_int(ctx, 10));
}

static BCValue cval_fromdb(BCContext *ctx, BCValue v1)
{
    return cval_exp(ctx, cval_mul(ctx, v1,
                                  cval_div(ctx, cdec_const(ctx, BC_CONST_LOG10),
                                           cint_from_int(ctx, 10))));
}

static BCValue cval_todeg(BCContext *ctx, BCValue v1)
{
    return cval_mul(ctx, v1, cval_div(ctx, cint_from_int(ctx, 180),
                                      cdec_const(ctx, BC_CONST_PI)));
}

static BCValue cval_fromdeg(BCContext *ctx, BCValue v1)
{
    return cval_mul(ctx, v1, cval_div(ctx, cdec_const(ctx, BC_CONST_PI),
                                      cint_from_int(ctx, 180)));
}

static BCValue cval_norm2(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) <= CTYPE_FLOAT) {
        return cval_mul(ctx, cval_dup(v1), v1);
    } else {
        return complex_norm2(ctx, v1);
    }
}
        
static BCValue cval_arg(BCContext *ctx, BCValue v1) 
{
    BCValue v;
    if (cval_type(v1) <= CTYPE_FLOAT) {
        v1 = to_complex(ctx, v1);
    }
    if (cval_type(v1) == CTYPE_COMPLEX) {
        v = cval_atan2(ctx, cval_dup(v1->u.complex.im), cval_dup(v1->u.complex.re));
        cval_free(ctx, v1);
        return v;
    } else {
        cval_free(ctx, v1);
        return cval_type_error(ctx, "incompatible type");
    }
}

static BCValue cval_deriv(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) == CTYPE_POLY) {
        return poly_deriv(ctx, v1);
    } else if (cval_type(v1) == CTYPE_RFRAC) {
        return rfrac_deriv(ctx, v1);
    } else  if (cval_type(v1) == CTYPE_SER) {
        return ser_deriv(ctx, v1);
    } else
    {
        return cval_type_error(ctx, "incompatible type");
    }
}

static BCValue cval_integ(BCContext *ctx, BCValue v1)
{
    if (cval_type(v1) == CTYPE_POLY) {
        return poly_integ(ctx, v1);
    } else if (cval_type(v1) == CTYPE_SER) {
        return ser_integ(ctx, v1);
    } else
    {
        return cval_type_error(ctx, "incompatible type");
    }
}

/* XXX: implement with libbf to get more precision */
static BCValue cval_sinc(BCContext *ctx, BCValue x)
{
    BCValue r;
    
    x = to_dec(ctx, x);
    if (cval_is_error(x))
        return x;
    if (cval_cmp_eq_int(ctx, x, 0)) {
        r = to_dec(ctx, cint_from_int(ctx, 1));
    } else {
        x = cval_mul(ctx, x, cdec_pi(ctx));
        r = cval_div(ctx, cval_sin(ctx, cval_dup(x)), cval_dup(x));
    }
    cval_free(ctx, x);
    return r;
}

static BCValue cval_num(BCContext *ctx, BCValue v1)
{
    BCValue v;
    
    if (cval_type(v1) == CTYPE_FRAC)
        v = cval_dup(v1->u.frac.num);
    else if (cval_type(v1) == CTYPE_RFRAC)
        v = cval_dup(v1->u.rfrac.num);
    else
        v = cval_type_error(ctx, "incompatible type");
    cval_free(ctx, v1);
    return v;
}

static BCValue cval_den(BCContext *ctx, BCValue v1)
{
    BCValue v;
    
    if (cval_type(v1) == CTYPE_FRAC)
        v = cval_dup(v1->u.frac.den);
    else if (cval_type(v1) == CTYPE_RFRAC)
        v = cval_dup(v1->u.rfrac.den);
    else
        v = cval_type_error(ctx, "incompatible type");
    cval_free(ctx, v1);
    return v;
}


static BCValue cval_shl(BCContext *ctx, BCValue v1, BCValue v2)
{
    if (cval_type(v2) != CTYPE_INT) {
        cval_free(ctx, v1);
        cval_free(ctx, v2);
        return cval_type_error(ctx, "right argument in shifts must be an integer");
    }
    return cint_shl(ctx, v1, v2);
}

static BCValue cval_not(BCContext *ctx, BCValue v1)
{
    BCValue v;
    if (cval_type(v1) != CTYPE_INT) {
        v = cval_type_error(ctx, "operands must be integer");
    } else {
        v = cint_new(ctx);
        bf_add_si(&v->u.cint, &v1->u.cint, 1, BF_PREC_INF, BF_RNDZ);
        bf_neg(&v->u.cint);
    }
    cval_free(ctx, v1);
    return v;
}

/* call a function. The arguments are freed. */
static BCValue cval_call(BCContext *ctx, BCValue func_val,
                          int n_args, BCValue *args)
{
    BCValue val;
    BCFunction *f;
    int i;

    if (cval_type(func_val) == CTYPE_POLY) {
        if (n_args != 1) {
            val = cval_type_error(ctx, "one argument expected");
            goto fail;
        }
        return poly_apply(ctx, func_val, args[0]);
    } else if (cval_type(func_val) == CTYPE_RFRAC) {
        if (n_args != 1) {
            val = cval_type_error(ctx, "one argument expected");
            goto fail;
        }
        return rfrac_apply(ctx, func_val, args[0]);
    } else if (cval_type(func_val) == CTYPE_SER) {
        if (n_args != 1) {
            val = cval_type_error(ctx, "one argument expected");
            goto fail;
        }
        return ser_apply(ctx, func_val, args[0]);
    } else if (cval_type(func_val) != CTYPE_FUNCTION) {
        val = cval_type_error(ctx, "function expected");
        goto fail;
    }
    f = &func_val->u.function;
    if (f->var_args) {
        if (n_args < f->nb_args) {
            val = cval_type_error(ctx, "at least %d argument%s expected",
                                  f->nb_args, f->nb_args >= 2 ? "s" : "");
            goto fail;
        }
    } else {
        if (n_args != f->nb_args) {
            val = cval_type_error(ctx, "%d argument%s expected",
                                  f->nb_args, f->nb_args >= 2 ? "s" : "");
        fail:
            cval_free(ctx, func_val);
            for(i = 0; i < n_args; i++)
                cval_free(ctx, args[i]);
            return val;
        }
    }

    if (f->var_args) {
        val = f->u.cfunc_vararg(ctx, n_args, args);
    } else {
        switch(n_args) {
        case 0:
            val = f->u.cfunc0(ctx);
            break;
        case 1:
            val = f->u.cfunc1(ctx, args[0]);
            break;
        case 2:
            val = f->u.cfunc2(ctx, args[0], args[1]);
            break;
        case 3:
            val = f->u.cfunc3(ctx, args[0], args[1], args[2]);
            break;
        default:
            abort();
        }
    }
    cval_free(ctx, func_val);
    return val;
}

static BCValue cval_getitem(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue tab, val;
    int i;
    
    tab = args[0];
    if (cval_type(tab) == CTYPE_ARRAY) {
        val = carray_getitem(ctx, n_args, args);
    } else if (cval_type(tab) == CTYPE_TENSOR) {
        val = tensor_getitem(ctx, n_args, args);
    } else if (cval_type(tab) == CTYPE_STRING) {
        val = cstring_getitem(ctx, n_args, args);
    } else if (cval_type(tab) == CTYPE_POLY) {
        val = poly_getitem(ctx, n_args, args);
    } else if (cval_type(tab) == CTYPE_SER) {
        val = ser_getitem(ctx, n_args, args);
    } else {
        for(i = 0; i < n_args; i++)
            cval_free(ctx, args[i]);
        val = cval_type_error(ctx, "getitem is not supported for this type");
    }
    return val;
}

static BCValue cval_setitem(BCContext *ctx, int n_args, BCValue *args)
{
    BCValue tab, val;
    int i;
    
    tab = args[0];
    if (cval_type(tab) == CTYPE_ARRAY) {
        val = carray_setitem(ctx, n_args, args);
    } else if (cval_type(tab) == CTYPE_TENSOR) {
        val = tensor_setitem(ctx, n_args, args);
    } else {
        for(i = 0; i < n_args; i++)
            cval_free(ctx, args[i]);
        val = cval_type_error(ctx, "setitem is not supported for this type");
    }
    return val;
}

#if 0
/****************************************************/
/* solver */

function cval_solve(var_str, func_str, x0)
{
    var x1, x2, i, y0, y1, eps;

    eps = float_new(1e-10);

    var_str = to_str(var_str);
    
    if (func_str.type != CTYPE_STRING)
        cval_error(CERR_TYPE);

    x0 = to_float(x0);

    /* guess a second point (XXX: could add a parameter) */
    if (cval_is_zero(x0)) {
        x1 = float_new(0.01);
    } else {
        x1 = cval_mul(x0, float_new(1.01));
    }

    set_variable(var_str, x0, true);
    val = eval_formula(func_str);
    y0 = to_float(val);
    for(i = 0; i < 40; i++) {
        //        log_str("x0=" + cval_toString(x0) + " x1=" + cval_toString(x1) + "\n");
        if (cval_cmp(cval_abs(cval_sub(x1, x0)), eps) < 0)
            return x1;
        set_variable(var_str, x1, true);
        val = eval_formula(func_str);
        y1 = to_float(val);
        //        log_str("y0=" + cval_toString(y0) + " y1=" + cval_toString(y1) + "\n");
        
        /* compute next point */
        x2 = cval_sub(x1, cval_mul(cval_div(cval_sub(x1, x0), 
                                            cval_sub(y1, y0)),
                                   y1));
        x0 = x1;
        x1 = x2;
        y0 = y1;
    }
    cval_error(CERR_ROOTS);
}
#endif

/****************************************************/
/* unit conversion */

typedef struct {
    const char *name;
    const char *abbrev;
    double val;
} PrefixDef;

typedef struct {
    const char *name;
    const char *names; /* NULL if equal to name + "s" */
    const char *abbrev;
    double val; /* value using "unit" */
    const char *unit; 
    const char *cat; /* category (for help) */
    double addend; /* only for temperature */
} UnitDef;

static const PrefixDef prefix_table[] = {
    { .name =  "yocto", .abbrev =  "y", .val =  1e-24 },
    { .name =  "zepto", .abbrev =  "z", .val =  1e-21 },
    { .name =  "atto", .abbrev =  "a", .val =  1e-18 },
    { .name =  "femto", .abbrev =  "f", .val =  1e-15 },
    { .name =  "pico", .abbrev =  "p", .val =  1e-12 },
    { .name =  "nano", .abbrev =  "n", .val =  1e-9 },
    { .name =  "micro", .abbrev =  "µ", .val =  1e-6 },
    { .name =  "milli", .abbrev =  "m", .val =  1e-3 },
    { .name =  "centi", .abbrev =  "c", .val =  1e-2 },
    { .name =  "deci", .abbrev =  "d", .val =  1e-1 },

    /* .XXX =  see if potential conflicts. In this case, it would be
       better to test them after */
    { .name =  "kibi", .abbrev =  "Ki", .val =  (double)((uint64_t)1 << (10 * 1)) },
    { .name =  "mebi", .abbrev =  "Mi", .val =  (double)((uint64_t)1 << (10 * 2)) },
    { .name =  "gibi", .abbrev =  "Gi", .val =  (double)((uint64_t)1 << (10 * 3)) },
    { .name =  "tebi", .abbrev =  "Ti", .val =  (double)((uint64_t)1 << (10 * 4)) },
    { .name =  "pebi", .abbrev =  "Pi", .val =  (double)((uint64_t)1 << (10 * 5)) },
    { .name =  "exbi", .abbrev =  "Ei", .val =  (double)((uint64_t)1 << (10 * 6)) },
    { .name =  "zebi", .abbrev =  "Zi", .val =  1180591620717411303424.0 /* 2^70 */ },
    { .name =  "yobi", .abbrev =  "Yi", .val =  1208925819614629174706176.0 /* 2^80 */ },

    { .name =  "deca", .abbrev =  "da", .val =  10 },
    { .name =  "hecto", .abbrev =  "h", .val =  100 },
    { .name =  "kilo", .abbrev =  "k", .val =  1000 },
    { .name =  "mega", .abbrev =  "M", .val =  1e6 },
    { .name =  "giga", .abbrev =  "G", .val =  1e9 },
    { .name =  "tera", .abbrev =  "T", .val =  1e12 },
    { .name =  "peta", .abbrev =  "P", .val =  1e15 },
    { .name =  "exa", .abbrev =  "E", .val =  1e18 },
    { .name =  "zetta", .abbrev =  "Z", .val =  1e21 },
    { .name =  "yotta", .abbrev =  "Y", .val =  1e24 },
};

static const char *prefix_power[2] = { "square", "cubic" };

#define N_BASE_UNITS 7
#define UNIT_NAME_SIZE 64

static const char *base_units[N_BASE_UNITS] = { "m", "kg", "s", "A", "K", "mol", "cd" };

static const UnitDef unit_table[] = {
    /* length */
    { .name =  "meter", .abbrev =  "m", .val =  1, .unit =  "m", .cat =  "Length"  },
    { .name =  "foot", .names =  "feet", .abbrev =  "ft", .val =  0.3048, .unit =  "m" },
    { .name =  "inch", .names =  "inches", .abbrev =  "in", .val =  0.0254, .unit =  "m" },
    { .name =  "mil", .abbrev =  "mil", .val =  1e-3, .unit =  "in" },
    { .name =  "mile", .abbrev =  "mi", .val =  5280, .unit =  "ft" },
    { .name =  "micron", .abbrev =  "", .val =  1e-6, .unit =  "m" },
    { .name =  "nautical mile", .abbrev =  "NM", .val =  1852, .unit =  "m" },
    { .name =  "", .abbrev =  "nmi", .val =  1852, .unit =  "m" },
    { .name =  "angstrom", .abbrev =  "", .val =  1e-10, .unit =  "m" },
    { .name =  "light year", .abbrev =  "ly", .val =  9.4607304725808e15, .unit =  "m" },
    { .name =  "parsec", .abbrev =  "pc", .val =  3.08567782e16, .unit =  "m" },
    { .name =  "astronomical unit", .abbrev =  "AU", .val =  149597870691, .unit =  "m" },
    
    /* surface */
    { .name =  "acre", .abbrev =  "ac", .val =  4046.8564224, .unit =  "m^2", .cat =  "Surface" },
    { .name =  "are", .abbrev =  "a", .val =  100, .unit =  "m^2" },

    /* volume */
    { .name =  "liter", .abbrev =  "L", .val =  1, .unit =  "dm^3", .cat =  "Volume" },

    /* time */
    { .name =  "second", .abbrev =  "s", .val =  1, .unit =  "s", .cat =  "Time" },
    { .name =  "minute", .abbrev =  "min", .val =  60, .unit =  "s" },
    { .name =  "hour", .abbrev =  "h", .val =  3600, .unit =  "s" },
    { .name =  "day", .abbrev =  "d", .val =  24, .unit =  "h" },
    { .name =  "week", .abbrev =  "wk", .val =  7, .unit =  "day" },
    { .name =  "month", .abbrev =  "mo", .val =  30, .unit =  "day" },
    { .name =  "year", .abbrev =  "y", .val =  365.25, .unit =  "day" },
    { .name =  "hertz", .abbrev =  "Hz", .val =  1, .unit =  "s^-1" },

    /* speed */
    { .name =  "", .abbrev =  "fph", .val =  1, .unit =  "ft/h", .cat =  "Speed" },
    { .name =  "", .abbrev =  "mph", .val =  1, .unit =  "mi/h" },
    { .name =  "speed of light", .abbrev =  "c", .val =  2.99792458e8, .unit =  "m/s" },
    { .name =  "knot", .abbrev =  "kn", .val =  1, .unit =  "NM/h" },

    /* acceleration */
    { .name =  "gravity", .abbrev =  "G", .val =  9.80665, .unit =  "m/s^2", .cat =  "Acceleration" },

    /* pressure */
    { .name =  "pascal", .abbrev =  "Pa", .val =  1, .unit =  "N/m^2", .cat =  "Pressure" },
    { .name =  "atmosphere", .abbrev =  "atm", .val =  101325, .unit =  "Pa" },
    { .name =  "bar", .abbrev =  "bar", .val =  1e5, .unit =  "Pa" },
    { .name =  "torr", .abbrev =  "torr", .val =  101325.0/760, .unit =  "Pa" },
    
    /* mass */
    { .name =  "gramm", .abbrev =  "g", .val =  1e-3, .unit =  "kg", .cat =  "Mass" },
    { .name =  "tonne", .abbrev =  "t", .val =  1000, .unit =  "kg" },
    { .name =  "carat", .abbrev =  "ct", .val =  0.2, .unit =  "g" },
    { .name =  "pound", .abbrev =  "lb", .val =  0.45359237, .unit =  "kg" },
    { .name =  "ounce", .abbrev =  "oz", .val =  1.0/16, .unit =  "lb" },

    /* temperature */
    { .name =  "kelvin", .abbrev =  "K", .val =  1, .unit =  "K", .cat =  "Temperature" },
    { .name =  "degree Celsius", .abbrev =  "°C", .val =  1, .unit =  "K", .addend =  273.15 },
    { .name =  "degree Rankine", .abbrev =  "°R", .val =  5.0/9, .unit =  "K" },
    { .name =  "degree Farenheit", .abbrev =  "°F", .val =  5.0/9, .unit =  "K", .addend =  459.67 },
   
    /* energy */
    { .name =  "joule", .abbrev =  "J", .val =  1, .unit =  "kg*m^2*s^-2", .cat =  "Energy" },
    { .name =  "electronvolt", .abbrev =  "eV", .val =  1.602176e-19, .unit =  "J" },
    { .name =  "calorie", .abbrev =  "cal", .val =  4.1868, .unit =  "J" },
    { .name =  "Calorie", .abbrev =  "Cal", .val =  4.1868e3, .unit =  "J" },
    { .name =  "ton of TNT", .abbrev =  "tTNT", .val =  4.184, .unit =  "GJ" },
    { .name =  "ton of oil equivalent", .abbrev =  "TOE", .val =  41.868, .unit =  "GJ" },
    
    /* power */
    { .name =  "watt", .abbrev =  "W", .val =  1, .unit =  "J/s", .cat =  "Power" },
    { .name =  "horsepower", .abbrev =  "hp", .val =  735.49875, .unit =  "W" },

    /* force */
    { .name =  "newton", .abbrev =  "N", .val =  1, .unit =  "kg*m*s^-2", .cat =  "Force" },
    
    /* electric current */
    { .name =  "ampere", .abbrev =  "A", .val =  1, .unit =  "A", .cat =  "Electric current" },
    
    /* electric charge */
    { .name =  "coulomb", .abbrev =  "C", .val =  1, .unit =  "A*s", .cat =  "Electric charge" },

    /* electric capacitance */
    { .name =  "farad", .abbrev =  "F", .val =  1, .unit =  "C/V", .cat =  "Electric capactiance" },

    /* Electromotive force */
    { .name =  "volt", .abbrev =  "V", .val =  1, .unit =  "kg*m^2*A^-1*s^-3", .cat =  "Electromotive force" },

    /* Electrical resistance */
    { .name =  "ohm", .abbrev =  "", .val =  1, .unit =  "V/A", .cat =  "Electrical resistance" },

    /* electrical conductance */
    { .name =  "siemens", .abbrev =  "S", .val =  1, .unit =  "A/V", .cat =  "Electrical conductance" },
    
    /* magnetic flux */
    { .name =  "weber", .abbrev =  "Wb", .val =  1, .unit =  "J/A", .cat =  "Magnetic flux" },

    /* magnetic field */
    { .name =  "tesla", .abbrev =  "T", .val =  1, .unit =  "Wb/m^2", .cat =  "Magnetic field" },

    /* inductance */
    { .name =  "henry", .abbrev =  "H", .val =  1, .unit =  "Wb/A", .cat =  "Inductance" },
    
    /* luminous flux */
    { .name =  "lumen", .abbrev =  "lm", .val =  1, .unit =  "cd*sr", .cat =  "Luminous flux" },
    
    /* illuminance */
    { .name =  "lux", .abbrev =  "lx", .val =  1, .unit =  "lm/m^2", .cat =  "Illuminance" },

    /* radioactivity (decays per unit time) */
    { .name =  "becquerel", .abbrev =  "Bq", .val =  1, .unit =  "s^-1", .cat =  "Radioactivity" },
    
    /* absorbed dose (of ionizing radiation) */
    { .name =  "gray", .abbrev =  "Gy", .val =  1, .unit =  "J/kg", .cat =  "Absorbed dose" },

    /* equivalent dose (of ionizing radiation) */
    { .name =  "sievert", .abbrev =  "Sv", .val =  1, .unit =  "J/kg", .cat =  "Equivalent dose" },

    /* catalytic activity */
    { .name =  "katal", .abbrev =  "kat", .val =  1, .unit =  "mol/s", .cat =  "Catalytic activity" },

    /* angle */
    { .name =  "radian", .abbrev =  "rad", .val =  1, .unit =  "rad", .cat =  "Angle" },
    { .name =  "degree", .abbrev =  "°", .val =  M_PI/180, .unit =  "rad" },
    { .name =  "gradian", .abbrev =  "grad", .val =  M_PI/200, .unit =  "rad" },

    /* solid angle */
    { .name =  "steradian", .abbrev =  "sr", .val =  1, .unit =  "sr", .cat =  "Solid angle" },

    /* information */
    { .name =  "bit", .abbrev =  "bit", .val =  9.569940e-24, .unit =  "J/K", .cat =  "Information" },
    { .name =  "nibble", .abbrev =  "", .val =  4, .unit =  "bit" },
    { .name =  "byte", .abbrev =  "B", .val =  8, .unit =  "bit" },
};

static BOOL convert_is_space(int c)
{
    return c == ' ' || c == '\t';
}

static char *remove_spaces(char *buf, size_t buf_size, const char *str)
{
    const char *p = str;
    char *q;
    q = buf;
    while (*p != '\0') {
        if (!convert_is_space(*p)) {
            if ((q - buf) >= buf_size - 1)
                break;
            *q++ = *p;
        }
        p++;
    }
    *q = '\0';
    return buf;
}

static char *to_lower(char *str)
{
    char *p = str;
    while (*p != '\0') {
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
        p++;
    }
    return str;
}

/* return -1 if not found */
static int find_unit_name(const char *name, BOOL is_long)
{
    int i;
    char buf[UNIT_NAME_SIZE];
    const char *str;
    
    if (name[0] == '\0')
        return -1;
    if (is_long) {
        for(i = 0; i < countof(unit_table); i++) {
            const UnitDef *ud = &unit_table[i];
            /* XXX: precompute */
            remove_spaces(buf, sizeof(buf), ud->name); 
            str = to_lower(buf);
            if (!strcmp(name, str))
                return i;
            /* plural */
            if (ud->names) {
                str = ud->names;
            } else {
                pstrcat(buf, sizeof(buf), "s");
            }
            if (!strcmp(name, str))
                return i;
        }
    } else {
        for(i = 0; i < countof(unit_table); i++) {
            if (!strcmp(unit_table[i].abbrev, name))
                return i;
        }
    }
    return -1;
}

typedef struct {
    double val;
    int tab[N_BASE_UNITS];
    double addend; /* for temperatures */
} UnitValue;

static int find_unit(UnitValue *r, const char *unit);

/* return 0 if found and 'r' is filled, -1 if not found.  */
static int find_unit1(UnitValue *r, const char *unit)
{
    int i, j;
    char name[UNIT_NAME_SIZE];
    char namel[UNIT_NAME_SIZE];
    const char *str;
    
    remove_spaces(name, sizeof(name), unit);
    pstrcpy(namel, sizeof(namel), name);
    to_lower(namel);

    /* see if it is a base unit */
    for(i = 0; i < countof(base_units); i++) {
        if (!strcmp(name, base_units[i])) {
            r->val = 1;
            r->addend = 0;
            for(j = 0;j < N_BASE_UNITS; j++)
                r->tab[j] = 0;
            r->tab[i] = 1;
            return 0;
        }
    }
    
    /* handle zero dimensions units */
    if (!strcmp(name, "rad") || !strcmp(name, "sr")) {
        r->val = 1;
        r->addend = 0;
        for(j = 0;j < N_BASE_UNITS; j++)
            r->tab[j] = 0;
        return 0;
    }
       
    /* see if exact abbreviation or name found */

    if ((i = find_unit_name(name, FALSE)) >= 0 ||
        (i = find_unit_name(namel, TRUE)) >= 0) {
        if (find_unit(r, unit_table[i].unit))
            return -1;
        r->addend = unit_table[i].addend; /* specific hack for degrees */
        r->val *= unit_table[i].val;
        return 0;
    }
    
    /* try square or cubic + name */
    for(i = 0; i < countof(prefix_power); i++) {
        str = prefix_power[i];
        if (strstart(namel, prefix_power[i], &str)) {
            if (find_unit(r, str))
                return -1;
            int p = 2 + i;
            r->val = pow(r->val, p);
            for(j = 0; j < N_BASE_UNITS; j++) {
                r->tab[j] *= p;
            }
            return 0;
        }
    }

    /* not found: try a long prefix + name */
    for(j = 0; j < countof(prefix_table); j++) {
        if (strstart(namel, prefix_table[j].name, &str)) {
            i = find_unit_name(str, TRUE);
            if (i < 0)
                break;
            if (find_unit(r, unit_table[i].unit))
                return -1;
            r->val *= unit_table[i].val * prefix_table[j].val;
            return 0;
        }
    }

    /* try an abbreviated prefix + abbreviated unit */
    for(j = 0; j < countof(prefix_table); j++) {
        if (strstart(name, prefix_table[j].abbrev, &str)) {
            i = find_unit_name(str, FALSE);
            if (i < 0)
                break;
            if (find_unit(r, unit_table[i].unit))
                return -1;
            r->val *= unit_table[i].val * prefix_table[j].val;
            return 0;
        }
    }
    return -1;
}

/* syntax: unit[^n]*[...][/unit^n] */
/* return 0 if found and 'r' is filled, -1 if not found.  */
static int find_unit(UnitValue *r, const char *unit)
{
    const uint8_t *p;
    char name[UNIT_NAME_SIZE], *q;
    BOOL is_den, is_neg;
    int ret, po, k;
    UnitValue r1;
    
    p = (const uint8_t *)unit;
    
    /* extract units and powers */
    is_den = FALSE;
    ret = -1;
    for(;;) {
        q = name;
        while (*p != '^' && *p != '*' && *p != '/' && !(p[0] == 0xc2 && p[1] == 0xb2) && *p != '\0') {
            if ((q - name) < sizeof(name) - 1)
                *q++ = *p;
            p++;
        }
        *q = '\0';
        if (find_unit1(&r1, name))
            return -1;
        if (*p == 0xc2 && p[1] == 0xb2) {
            /* ² */
            p += 2;
            po = 2;
            goto handle_power;
        } else if (*p == '^') {
            p++;
            while (convert_is_space(*p))
                p++;
            is_neg = FALSE;
            if (*p == '+') {
                p++;
            } else if (*p == '-') {
                is_neg = TRUE;
                p++;
            }
            po = 0;
            while (*p >= '0' && *p <= '9') {
                /* XXX: overflow */
                po = po * 10 + (*p - '0');
                p++;
            }
            if (is_neg)
                po = -po;
        handle_power:
            r1.val = pow(r1.val, po);
            for(k = 0; k < N_BASE_UNITS; k++)
                r1.tab[k] *= po;
        }
        if (!is_den) {
            if (ret < 0) {
                ret = 0;
                *r = r1;
            } else {
                r->val *= r1.val;
                for(k = 0; k < N_BASE_UNITS; k++)
                    r->tab[k] += r1.tab[k];
            }
        } else {
            r->val /= r1.val;
            for(k = 0; k < N_BASE_UNITS; k++)
                r->tab[k] -= r1.tab[k];
        }

        while (convert_is_space(*p))
            p++;
        if (*p == '*') {
            p++;
        } else if (*p == '/') {
            p++;
            is_den = TRUE;
        } else {
            if (*p != '\0')
                return -1;
            break;
        }
    }
    return ret;
}

static void unit_dims(DynBuf *d, const UnitValue *r)
{
    int i;
    BOOL is_first = TRUE;
    for(i=0;i<N_BASE_UNITS;i++) {
        if (r->tab[i] != 0) {
            if (!is_first)
                dbuf_putc(d, '*');
            dbuf_printf(d, "%s^%d", base_units[i], r->tab[i]);
            is_first = FALSE;
        }
    }
    if (is_first) {
        dbuf_putstr(d, "unitless");
    }
}

static BOOL is_temperature(const UnitValue *r)
{
    int i, v;
    for(i = 0; i < N_BASE_UNITS; i++) {
        if (i == 4)
            v = 1;
        else
            v = 0;
        if (r->tab[i] != v) 
            return FALSE;
    }
    return TRUE;
}

/* XXX: could increase the precision */
static BCValue cval_convert_unit(BCContext *ctx, BCValue v1, BCValue unit1, BCValue unit2)
{
    int i;
    UnitValue r1, r2;
    double val;
    BCValue v;
    BOOL is_float;
    
    if (cval_type(unit1) != CTYPE_STRING ||
        cval_type(unit2) != CTYPE_STRING) {
        cval_type_error(ctx, "string expected");
        goto fail;
    }
    is_float = (cval_type(v1) == CTYPE_FLOAT);
    v1 = to_float1(ctx, v1, TRUE);
    if (cval_is_error(v1))
        goto fail;
    bf_get_float64(&v1->u.cfloat, &val, BF_RNDN);
    
    if (find_unit(&r1, (char *)unit1->u.string.str)) {
        cval_syntax_error(ctx, "unknown unit: '%s'", (char *)unit1->u.string.str);
        goto fail;
    }
    if (find_unit(&r2, (char *)unit2->u.string.str)) {
        cval_syntax_error(ctx, "unknown unit: '%s'", (char *)unit2->u.string.str);
        goto fail;
    }

    /* check dimensions */
    for(i = 0; i < N_BASE_UNITS; i++) {
        if (r1.tab[i] != r2.tab[i]) {
            DynBuf dbuf;
            dbuf_init(&dbuf);
            dbuf_putstr(&dbuf, "Unit dimensions do not match: ");
            unit_dims(&dbuf, &r1);
            dbuf_putstr(&dbuf, " != ");
            unit_dims(&dbuf, &r2);
            dbuf_putc(&dbuf, '\0');
            cval_range_error(ctx, (char *)dbuf.buf);
            dbuf_free(&dbuf);
            goto fail;
        }
    }

    if (is_temperature(&r1)) {
        /* specific case for degrees */
        /* convert to kelvins */
        val += r1.addend;
        val = val * r1.val / r2.val;
        val -= r2.addend;
    } else {
        val = val * r1.val / r2.val;
    }
    v = cfloat_from_double(ctx, val);
    if (!is_float)
        v = to_dec1(ctx, v, TRUE); /* decimal by default */
 done:
    cval_free(ctx, v1);
    cval_free(ctx, unit1);
    cval_free(ctx, unit2);
    return v;
 fail:
    v = BC_EXCEPTION;
    goto done;
}

/********************************************/
/* bc_context */

static BCVarDef *find_variable(BCContext *ctx, const char *name)
{
    struct list_head *el;
    list_for_each(el, &ctx->var_list) {
        BCVarDef *ve = list_entry(el, BCVarDef, link);
        if (!strcmp(ve->name, name))
            return ve;
    }
    return NULL;
}

static BCValue get_variable(BCContext *ctx, const char *name)
{
    BCVarDef *ve;
    ve = find_variable(ctx, name);
    if (!ve)
        return cval_throw_error(ctx, CERR_REFERENCE, "variable '%s' is not defined", name);
    if (ve->is_getter) {
        return cval_call(ctx, cval_dup(ve->value), 0, NULL);
    } else {
        return cval_dup(ve->value);
    }
}

static void set_variable(BCContext *ctx, const char *name, BCValueConst val1,
                         BOOL is_getter, BOOL is_user)
{
    BCVarDef *ve;
    BCValue val;
    ve = find_variable(ctx, name);
    if (!ve) {
        ve = mallocz(sizeof(*ve));
        ve->name = strdup(name);
        ve->is_user = FALSE;
        ve->value = NULL;
        list_add_tail(&ve->link, &ctx->var_list);
    }
    val = cval_dup(val1);
    if (ve->value)
        cval_free(ctx, ve->value);
    ve->is_getter = is_getter;
    ve->value = val;
    ve->is_user |= is_user;
}

static BCValue func_new(BCContext *ctx, const char *name, void *cfunc,
                        int nb_args, BOOL var_args)
{
    BCValue v;
    v = cval_new(ctx, CTYPE_FUNCTION);
    assert(nb_args <= FUNCTION_MAX_ARGS);
    v->u.function.nb_args = nb_args;
    v->u.function.var_args = var_args;
    v->u.function.u.cfunc = cfunc;
    v->u.function.name = strdup(name);
    return v;
}

static void def_func2(BCContext *ctx, const char *name, void *cfunc,
                      int nb_args, BOOL var_args)
{
    BCValue v;
    v = func_new(ctx, name, cfunc, nb_args, var_args);
    set_variable(ctx, name, v, FALSE, FALSE);
    cval_free(ctx, v);
}

static void def_func(BCContext *ctx, const char *name, void *cfunc, int nb_args)
{
    def_func2(ctx, name, cfunc, nb_args, FALSE);
}

static void add_functions(BCContext *ctx)
{
    def_func(ctx, "Integer", to_cint, 1);
    def_func(ctx, "int", to_cint, 1);
    def_func(ctx, "neg", cval_neg, 1);
    def_func(ctx, "inverse", cval_inverse, 1);
    def_func(ctx, "norm2", cval_norm2, 1);
    def_func(ctx, "abs", cval_abs, 1);
    def_func(ctx, "trunc", cval_trunc, 1);
    def_func(ctx, "floor", cval_floor, 1);
    def_func(ctx, "ceil", cval_ceil, 1);
    def_func(ctx, "round", cval_round, 1);
    def_func(ctx, "num", cval_num, 1);
    def_func(ctx, "den", cval_den, 1);

    def_func(ctx, "fact", cval_fact, 1);
    def_func(ctx, "comb", cval_comb, 2);
    def_func(ctx, "xor", cval_xor, 2);
    def_func(ctx, "divrem", cval_divrem, 2);
    def_func(ctx, "gcd", cval_gcd, 2);
    def_func(ctx, "sqrt", cval_sqrt, 1);

    def_func(ctx, "conj", cval_conj, 1);
    def_func(ctx, "re", cval_re, 1);
    def_func(ctx, "im", cval_im, 1);
    def_func(ctx, "arg", cval_arg, 1);
    def_func(ctx, "invmod", cint_invmod, 2);
    def_func(ctx, "pmod", cint_pmod, 3);
    def_func(ctx, "ilog2", cint_ilog2, 1);
    def_func(ctx, "ctz", cint_ctz, 1);
    def_func2(ctx, "isprime", cint_isprime, 1, TRUE);
    def_func(ctx, "nextprime", cint_nextprime, 1);
    def_func(ctx, "factor", cint_factor, 1);
    def_func(ctx, "bestappr", cval_bestappr, 2);

    /* transcendental */
    def_func(ctx, "exp", cval_exp, 1);
    def_func(ctx, "log", cval_log, 1);
    def_func(ctx, "log2", cval_log2, 1);
    def_func(ctx, "log10", cval_log10, 1);

    def_func(ctx, "sin", cval_sin, 1);
    def_func(ctx, "cos", cval_cos, 1);
    def_func(ctx, "tan", cval_tan, 1);
    def_func(ctx, "asin", cval_asin, 1);
    def_func(ctx, "acos", cval_acos, 1);
    def_func(ctx, "atan", cval_atan, 1);
    def_func(ctx, "atan2", cval_atan2, 2);

    def_func(ctx, "sinh", cval_sinh, 1);
    def_func(ctx, "cosh", cval_cosh, 1);
    def_func(ctx, "tanh", cval_tanh, 1);
    def_func(ctx, "asinh", cval_asinh, 1);
    def_func(ctx, "acosh", cval_acosh, 1);
    def_func(ctx, "atanh", cval_atanh, 1);

    def_func(ctx, "sinc", cval_sinc, 1);
    def_func(ctx, "todb", cval_todb, 1);
    def_func(ctx, "fromdb", cval_fromdb, 1);
    def_func(ctx, "todeg", cval_todeg, 1);
    def_func(ctx, "fromdeg", cval_fromdeg, 1);

    def_func(ctx, "Fraction", cfrac_new, 2);
    def_func(ctx, "Decimal", cdec_ctor, 1);
    def_func(ctx, "Float", cfloat_ctor, 1);
    def_func(ctx, "Complex", complex_new, 2);

    /* array */
    def_func2(ctx, "Array", carray_ctor, 0, TRUE);
    def_func(ctx, "len", cval_len, 1);

    /* string */
    def_func(ctx, "chr", cstring_chr, 1);
    def_func(ctx, "ord", cstring_ord, 1);
    
    /* tensor */
    def_func(ctx, "Tensor", tensor_ctor, 1);
    def_func2(ctx, "zeros", tensor_zeros, 0, TRUE);
    def_func(ctx, "shape", tensor_shape, 1);

    /* matrix */
    def_func(ctx, "idn", matrix_idn, 1);
    def_func(ctx, "diag", matrix_diag, 1);
    def_func(ctx, "mathilbert", mathilbert, 1);
    def_func(ctx, "trace", matrix_trace, 1);
    def_func(ctx, "trans", matrix_trans, 1);
    def_func(ctx, "charpoly", matrix_charpoly, 1);
    def_func(ctx, "dp", vector_dp, 2);
    def_func(ctx, "cp", vector_cp, 2);
    def_func(ctx, "det", matrix_det, 1);
    def_func(ctx, "rank", matrix_rank, 1);
    def_func(ctx, "ker", matrix_ker, 1);
    def_func(ctx, "eigenvals", matrix_eigenvals, 1);

    /* polynomial */
    def_func(ctx, "Polynomial", poly_ctor, 1);
    def_func(ctx, "deg", cval_deg, 1);
    def_func(ctx, "deriv", cval_deriv, 1);
    def_func(ctx, "integ", cval_integ, 1);
    def_func(ctx, "primpart", poly_primpart, 1);

    def_func(ctx, "RationalFunction", rfrac_new, 2);
    
    def_func(ctx, "O", ser_O, 1);
    def_func2(ctx, "polroots", poly_roots, 1, TRUE);

#if 0
    /* misc */
    def_func(ctx, "solve", cval_solve, 3);
#endif    
    def_func(ctx, "convert", cval_convert_unit, 3);
    def_func(ctx, "typeof", cval_typeof, 1);

    /* constants */
    {
        BCValue v;
        v = complex_new(ctx, cint_from_int(ctx, 0), cint_from_int(ctx, 1));
        set_variable(ctx, "I", v, FALSE, FALSE);
        cval_free(ctx, v);

        v = cval_div(ctx, cint_from_int(ctx, 1), cint_from_int(ctx, 0));
        set_variable(ctx, "Inf", v, FALSE, FALSE);
        cval_free(ctx, v);

        v = cval_div(ctx, cint_from_int(ctx, 0), cint_from_int(ctx, 0));
        set_variable(ctx, "NaN", v, FALSE, FALSE);
        cval_free(ctx, v);
        
        v = func_new(ctx, "PI", cdec_pi, 0, FALSE);
        set_variable(ctx, "PI", v, TRUE, FALSE);
        cval_free(ctx, v);

        v = poly_new_X(ctx);
        set_variable(ctx, "X", v, FALSE, FALSE);
        cval_free(ctx, v);
    }
}

BCContext *bc_context_new(void)
{
    BCContext *ctx;
    int i;
    
    ctx = mallocz(sizeof(*ctx));
    bf_context_init(&ctx->bf_ctx, my_bf_realloc, NULL);
    init_list_head(&ctx->var_list);
    for(i = 0; i < CTYPE_COUNT; i++) {
        if (!has_elem_type(i)) {
            ctx->def_type[i] = ctype_new(ctx, i, NULL);
        }
    }

    ctx->null_value = cval_new(ctx, CTYPE_NULL);
    for(i = 0; i < 2; i++) {
        ctx->bool_value[i] = cval_new(ctx, CTYPE_BOOL);
        ctx->bool_value[i]->u.bool_val = i;
    }
    
#if 0
    ctx->float_prec = 113;
    ctx->float_flags = bf_set_exp_bits(15) | BF_RNDN | BF_FLAG_SUBNORMAL;
    ctx->dec_prec = 34;
    ctx->dec_flags = bf_set_exp_bits(15) | BF_RNDN | BF_FLAG_SUBNORMAL;
#else
    ctx->float_prec = 53;
    ctx->float_flags = bf_set_exp_bits(11) | BF_RNDN | BF_FLAG_SUBNORMAL;
    ctx->dec_prec = 16;
    ctx->dec_flags = bf_set_exp_bits(11) | BF_RNDN | BF_FLAG_SUBNORMAL;
#endif

    add_functions(ctx);

    ctx->tensor_output_lf = TRUE;
    return ctx;
}

void bc_context_free(BCContext *ctx)
{
    struct list_head *el, *el1;
    int i;
    
    list_for_each_safe(el, el1, &ctx->var_list) {
        BCVarDef *ve = list_entry(el, BCVarDef, link);
        free(ve->name);
        cval_free(ctx, ve->value);
        free(ve);
    }
    cval_free(ctx, ctx->null_value);
    for(i = 0; i < 2; i++)
        cval_free(ctx, ctx->bool_value[i]);
    for(i = 0; i < BC_CONST_COUNT; i++)
        cval_free(ctx, ctx->const_tab[i].value);
    
    for(i = 0; i < CTYPE_COUNT; i++) {
        if (ctx->def_type[i]) {
            ctype_free(ctx, ctx->def_type[i]);
        }
    }
    bf_context_end(&ctx->bf_ctx);
    free(ctx);
}

/********************************************/
/* expression parser */

static void __attribute__((format(printf, 2, 3), noreturn)) eval_error(ParseState *s, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cval_throw_error_buf(s->ctx, CERR_SYNTAX, buf);
    longjmp(s->jmp_env, 1);
}

/* raise the error if 'v' contains an error object */
static void check_error(ParseState *s, BCValue v)
{
    if (cval_is_error(v))
        longjmp(s->jmp_env, 1);
}

static void free_token(BCContext *ctx, Token *token)
{
    if (token->value != NULL) {
        cval_free(ctx, token->value);
        token->value = NULL;
    }
    token->val = TOK_EOF;
}

static inline int to_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    else if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    else
        return 36;
}

static void parse_number(ParseState *s, const char **pp)
{
    BCContext *ctx = s->ctx;
    const char *p;
    int base;
    BCValue v;
    BOOL is_float, is_bin_float, is_imag;
    DynBuf dbuf, *d = &dbuf;

    dbuf_init(d);
    is_float = FALSE;
    is_imag = FALSE;
    base = 10;
    p = *pp;
    if (*p == '0') {
        dbuf_putc(d, *p);
        p++;
        if (*p == 'x' || *p == 'X') {
            dbuf_putc(d, *p);
            p++;
            base = 16;
        } else if (*p == 'b' || *p == 'B') {
            dbuf_putc(d, *p);
            p++;
            base = 2;
        }
    }
    for(;;) {
        if (*p == '_' && to_digit(p[1]) < base)
            p++;
        if (to_digit(*p) >= base)
            break;
        dbuf_putc(d, *p);
        p++;
    }
    
    if (*p == '.') {
        is_float = TRUE;
        /* decimal point */
        dbuf_putc(d, *p);
        p++;
        for(;;) {
            if (*p == '_' && to_digit(p[1]) < base)
                p++;
            if (to_digit(*p) >= base)
                break;
            dbuf_putc(d, *p);
            p++;
        }
    }
    
    if ((base == 10 && (*p == 'e' || *p == 'E')) ||
        (base != 10 && (*p == 'p' || *p == 'P'))) { 
        is_float = TRUE;
        dbuf_putc(d, *p);
        p++;
        if (*p == '-' || *p == '+') {
            dbuf_putc(d, *p);
            p++;
        }
        if (to_digit(*p) >= 10) {
            dbuf_free(d);
            eval_error(s, "invalid number literal");
        }
        for(;;) {
            if (*p == '_' && to_digit(p[1]) < 10)
                p++;
            if (to_digit(*p) >= 10)
                break;
            dbuf_putc(d, *p);
            p++;
        }
    }
    dbuf_putc(d, '\0');
    is_bin_float = (is_float && base != 10);
    
    if (*p == 'l') {
        p++;
        is_float = TRUE;
        is_bin_float = TRUE;
    }
    if (*p == 'i') {
        p++;
        is_imag = TRUE;
    }
    
    if (is_float) {
        if (is_bin_float) {
            v = cfloat_new(s->ctx);
            bf_atof(&v->u.cfloat, (char *)d->buf, NULL, 0, ctx->float_prec,
                    ctx->float_flags | BF_ATOF_BIN_OCT | BF_ATOF_NO_NAN_INF);
        } else {
            v = cdec_new(s->ctx);
            bfdec_atof(&v->u.cdec, (char *)d->buf, NULL, ctx->dec_prec,
                       ctx->dec_flags | BF_ATOF_NO_NAN_INF);
        }
    } else {
        v = cint_new(s->ctx);
        bf_atof(&v->u.cint, (char *)d->buf, NULL, 0, BF_PREC_INF,
                BF_ATOF_BIN_OCT | BF_ATOF_NO_NAN_INF);
    }
    if (is_imag) {
        v = complex_new(ctx, cint_from_int(ctx, 0), v);
    }
    s->token.val = TOK_NUMBER;
    dbuf_free(d);
    *pp = p;
    s->token.value = v;
}

static void parse_ident(ParseState *s, const char **pp)
{
    const char *p;
    char *q;
    int tok;
    
    p = *pp;
    q = s->token.ident;
    while ((*p >= 'a' && *p <= 'z') ||
           (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') ||
           *p == '_') {
        if ((q - s->token.ident) >= sizeof(s->token.ident) - 1)
            eval_error(s, "identifier too long");
        *q++ = *p++;
    }
    *q = '\0';
    *pp = p;
    if (!strcmp(s->token.ident, "null"))
        tok = TOK_NULL;
    else if (!strcmp(s->token.ident, "false"))
        tok = TOK_FALSE;
    else if (!strcmp(s->token.ident, "true"))
        tok = TOK_TRUE;
    else
        tok = TOK_IDENT;
    s->token.val = tok;
}

static void parse_string(ParseState *s, const char **pp, int sep)
{
    const char *p;
    DynBuf dbuf;
    BCValue v;
    int c;
    
    dbuf_init(&dbuf);
    p = *pp;
    p++;
    for(;;) {
        c = *p;
        if (c == '\0')
            eval_error(s, "unexpected end of string");
        if (c == sep)
            break;
        if (*p == '\\') {
            p++;
            c = *p++;
            switch(c) {
            case '\0':
                eval_error(s, "unexpected end of string");
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'v':
                c = '\v';
                break;
            case '\'':
            case '\"':
            case '\\':
                break;
            default:
                eval_error(s, "unsupported string escape code");
            }
            dbuf_putc(&dbuf, c);
        } else {
            p++;
            dbuf_putc(&dbuf, c);
        }
    }
    dbuf_putc(&dbuf, '\0');
    p++;
    *pp = p;

    v = cval_new(s->ctx, CTYPE_STRING);
    v->u.string.len = dbuf.size - 1;
    v->u.string.str = dbuf.buf;
    s->token.value = v;
    s->token.val = TOK_STRING;
}

static void next_token(ParseState *s)
{
    const char *p;
    int c;
    
    free_token(s->ctx, &s->token);
    p = s->buf_ptr;
 redo:
    c = *p;
    switch(c) {
    case '\0':
        s->token.val = TOK_EOF;
        break;
    case ' ':
    case '\t':
    case '\f':
    case '\v':
    case '\r':
    case '\n':
        p++;
        goto redo;
    case '0' ... '9':
        parse_number(s, &p);
        break;
    case '\'':
    case '\"':
        parse_string(s, &p, c);
        break;
    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '_':
        parse_ident(s, &p);
        break;
    case '/':
        if (p[1] == '*') {
            /* comment */
            p += 2;
            for(;;) {
                if (*p == '\0')
                    eval_error(s, "unexpected end of comment");
                if (p[0] == '*' && p[1] == '/') {
                    p += 2;
                    break;
                }
                p++;
            }
            goto redo;
        } else if (p[1] == '/') {
            p += 2;
            s->token.val = TOK_DIV2;
        } else if (p[1] == '=') {
            p += 2;
            s->token.val = TOK_DIV_ASSIGN;
        } else {
            goto def_token;
        }
        break;
    case '*':
        if (p[1] == '=') {
            p += 2;
            s->token.val = TOK_MUL_ASSIGN;
        } else if (p[1] == '*') {
            if (p[2] == '=') {
                p += 3;
                s->token.val = TOK_POW_ASSIGN;
            } else {
                p += 2;
                s->token.val = TOK_POW;
            }
        } else {
            goto def_token;
        }
        break;
    case '^':
        if (p[1] == '^') {
            p += 2;
            s->token.val = TOK_XOR;
        } else {
            goto def_token;
        }
        break;
    case '<':
        if (p[1] == '=') {
            p += 2;
            s->token.val = TOK_LTE;
        } else if (p[1] == '<') {
            p += 2;
            s->token.val = TOK_SHL;
        } else {
            goto def_token;
        }
        break;
    case '>':
        if (p[1] == '=') {
            p += 2;
            s->token.val = TOK_GTE;
        } else if (p[1] == '>') {
            p += 2;
            s->token.val = TOK_SAR;
        } else {
            goto def_token;
        }
        break;
    case '=':
        if (p[1] == '=') {
            if (p[2] == '=') {
                p += 3;
                s->token.val = TOK_STRICT_EQ;
            } else {
                p += 2;
                s->token.val = TOK_EQ;
            }
        } else {
            goto def_token;
        }
        break;
    case '!':
        if (p[1] == '=') {
            if (p[2] == '=') {
                p += 3;
                s->token.val = TOK_STRICT_NEQ;
            } else {
                p += 2;
                s->token.val = TOK_NEQ;
            }
        } else {
            goto def_token;
        }
        break;
    case '.':
        if (p[1] == '*') {
            p += 2;
            s->token.val = TOK_DOT_MUL;
        } else {
            goto def_token;
        }
        break;
    default:
    def_token:
        p++;
        s->token.val = c;
        break;
    }

    s->buf_ptr = p;
}

#define EVAL_FLAG_TENSOR (1 << 0) /* in tensor definition */

static BCValue eval_expr(ParseState *s, int flags);

/* call a function */
static BCValue eval_function(ParseState *s, BCValue func_val)
{
    BCValue args[64], val;
    int n_args;
    
    next_token(s);
    n_args = 0;
    if (s->token.val == ')') {
        /* no arguments */
    } else {
        /* XXX: memory leak if error */
        for(;;) {
            if (n_args >= countof(args))
                eval_error(s, "too many function arguments");
            args[n_args++] = eval_expr(s, 0);
            if (s->token.val == ')') {
                break;
            } else if (s->token.val != ',') {
                eval_error(s, "comma or closing parenthesis expected");
            }
            next_token(s);
        }
    }
    next_token(s);

    val = cval_call(s->ctx, func_val, n_args, args);
    check_error(s, val);
    return val;
}

static int eval_subscripts(ParseState *s, BCValue *args)
{
    BCContext *ctx = s->ctx;
    BCValue start, stop;
    int n_dims;
    
    next_token(s);
    n_dims = 0;
    if (s->token.val != ']') {
        for(;;) {
            if (n_dims >= MAX_DIMS)
                eval_error(s, "too many indices");
            if (s->token.val != ':') {
                start = eval_expr(s, 0);
            } else {
                start = cnull_new(ctx);
            }
            if (s->token.val == ':') {
                next_token(s);
                if (s->token.val != ',' && s->token.val != ']') {
                    stop = eval_expr(s, 0);
                } else {
                    stop = cnull_new(ctx);
                }
                args[n_dims++] = crange_new(ctx, start, stop);
            } else {
                args[n_dims++] = start;
            }
            if (s->token.val == ']') {
                break;
            } else if (s->token.val != ',') {
                eval_error(s, "',' or ']' expected");
            }
            next_token(s);
        }
    }
    next_token(s);
    return n_dims;
}

static BCValue eval_array_literal(ParseState *s, int flags)
{
    BCContext *ctx = s->ctx;
    BCValue tab;
    int flags1;
    
    next_token(s);
    tab = carray_new(ctx, 0);
    if (s->token.val != ']') {
        for(;;) {
            flags1 = flags;
            if (flags & EVAL_FLAG_TENSOR) {
                if (s->token.val != '[')
                    flags1 &= ~EVAL_FLAG_TENSOR;
            }
            if (carray_push1(ctx, tab, eval_expr(s, flags1))) {
                check_error(s, BC_EXCEPTION);
            }
            if (s->token.val == ']')
                break;
            if (s->token.val != ',')
                eval_error(s, "comma expected");
            next_token(s);
        }
    }
    next_token(s);
    return tab;
}

enum {
    LVALUE_NONE,
    LVALUE_VAR,
    LVALUE_ITEM,
    LVALUE_PROP,
};

static BCValue eval_postfix_expr(ParseState *s, int flags)
{
    BCContext *ctx = s->ctx;
    BCValue val;
    char ident[IDENT_SIZE_MAX];
    BCValue dims[MAX_DIMS];
    int n_dims = 0;
    int lvalue = LVALUE_NONE;

    switch(s->token.val) {
    case TOK_NUMBER:
    case TOK_STRING:
        val = cval_dup(s->token.value);
        next_token(s);
        break;
    case TOK_IDENT:
        pstrcpy(ident, sizeof(ident), s->token.ident);
        next_token(s);
        lvalue = LVALUE_VAR;
        val = NULL;
        break;
    case TOK_NULL:
        val = cnull_new(ctx);
        next_token(s);
        break;
    case TOK_FALSE:
    case TOK_TRUE:
        val = cbool_new(ctx, s->token.val - TOK_FALSE);
        next_token(s);
        break;
    case '(':
        next_token(s);
        val = eval_expr(s, 0);
        if (s->token.val != ')')
            eval_error(s, "closing parenthesis expected");
        next_token(s);
        break;
    case '[':
        {
            BOOL is_tensor = FALSE;
            if (!ctx->js_mode && !(flags & EVAL_FLAG_TENSOR)) {
                is_tensor = TRUE;
                /* the flags is used to convert [[1,2],[2,3]] into Tensor([[1,2],[2,3]]) */
                flags |= EVAL_FLAG_TENSOR;
            }
            val = eval_array_literal(s, flags);
            if (is_tensor) {
                val = tensor_from_array(s->ctx, val);
                check_error(s, val);
            }
        }
        break;
    default:
        eval_error(s, "unexpected character in expression");
    }

    for(;;) {
        if (s->token.val == '=') {
            BCValue val2;
            if (lvalue == LVALUE_NONE)
                eval_error(s, "lvalue expected"); 
            next_token(s);
            val2 = eval_expr(s, 0);
            switch(lvalue) {
            case LVALUE_VAR:
                set_variable(ctx, ident, val2, FALSE, TRUE);
                break;
            case LVALUE_ITEM:
                {
                    BCValue *args;
                    int i;
                    args = malloc((n_dims + 2) * sizeof(args[0]));
                    args[0] = val;
                    for(i = 0; i < n_dims; i++)
                        args[1 + i] = dims[i];
                    args[1 + n_dims] = cval_dup(val2);
                    val = cval_setitem(ctx, n_dims + 2, args);
                    free(args);
                    check_error(s, val);
                    cval_free(ctx, val);
                }
                break;
            default:
                abort();
            }
            val = val2;
            break;
        } else {
            if (lvalue != LVALUE_NONE) {
                switch(lvalue) {
                case LVALUE_VAR:
                    val = get_variable(ctx, ident);
                    check_error(s, val);
                    break;
                case LVALUE_ITEM:
                    {
                        BCValue *args;
                        int i;
                        args = malloc((n_dims + 1) * sizeof(args[0]));
                        args[0] = val;
                        for(i = 0; i < n_dims; i++)
                            args[1 + i] = dims[i];
                        val = cval_getitem(ctx, n_dims + 1, args);
                        free(args);
                        check_error(s, val);
                    }
                    break;
                default:
                    abort();
                }
                lvalue = LVALUE_NONE;
            }
            if (s->token.val == '(') {
                val = eval_function(s, val);
            } else if (s->token.val == '[') {
                n_dims = eval_subscripts(s, dims);
                lvalue = LVALUE_ITEM;
            } else {
                break;
            }
        }
    }
    return val;
}

static BCValue eval_unary(ParseState *s, int flags)
{
    BCValue val;
    int op;

    switch(s->token.val) {
    case '-':
    case '+':
    case '~':
        /* unary */
        op = s->token.val;
        next_token(s);
        val = eval_unary(s, flags);
        if (op == '-')
            val = cval_neg(s->ctx, val);
        else if (op == '~')
            val = cval_not(s->ctx, val);
        check_error(s, val);
        break;
    default:
        val = eval_postfix_expr(s, flags);
        if (s->token.val == TOK_POW || (s->token.val == '^' && !s->ctx->js_mode)) {
            BCValue val2;
            next_token(s);
            val2 = eval_unary(s, flags);
            val = cval_pow(s->ctx, val, val2);
            check_error(s, val);
        }
        break;
    }
    return val;
}

static BCValue eval_expr_binary(ParseState *s, int flags, int level)
{
    BCValue val, val2;
    int op;

    if (level == 0)
        return eval_unary(s, flags);
    
    val = eval_expr_binary(s, flags, level - 1);
    for(;;) {
        op = s->token.val;
        switch(level) {
        case 1:
            switch(op) {
            case TOK_DIV2:
                break;
            default:
                return val;
            }
            break;
        case 2:
            switch(op) {
            case '*':
            case '/':
            case '%':
            case TOK_DOT_MUL:
                break;
            default:
                return val;
            }
            break;
        case 3:
            switch(op) {
            case '+':
            case '-':
                break;
            default:
                return val;
            }
            break;
        case 4:
            switch(op) {
            case TOK_SHL:
            case TOK_SAR:
                break;
            default:
                return val;
            }
            break;
        case 5:
            switch(op) {
            case '<':
            case '>':
            case TOK_LTE:
            case TOK_GTE:
                break;
            default:
                return val;
            }
            break;
        case 6:
            switch(op) {
            case TOK_EQ:
            case TOK_NEQ:
                break;
            default:
                return val;
            }
            break;
        case 7:
            switch(op) {
            case '&':
                break;
            default:
                return val;
            }
            break;
        case 8:
            if (op == TOK_XOR || (op == '^' && s->ctx->js_mode)) {
                op = TOK_XOR;
            } else {
                return val;
            }
            break;
        case 9:
            switch(op) {
            case '|':
                break;
            default:
                return val;
            }
            break;
        default:
            abort();
        }
        
        next_token(s);
        val2 = eval_expr_binary(s, flags, level - 1);

        switch(op) {
        case TOK_DIV2:
            val = cval_frac_div(s->ctx, val, val2);
            break;
        case '*':
            val = cval_mul(s->ctx, val, val2);
            break;
        case '/':
            val = cval_div(s->ctx, val, val2);
            break;
        case '%':
            val = cval_mod(s->ctx, val, val2);
            break;
        case TOK_DOT_MUL:
            val = cval_dot_mul(s->ctx, val, val2);
            break;
        case '+':
            val = cval_add(s->ctx, val, val2);
            break;
        case '-':
            val = cval_sub(s->ctx, val, val2);
            break;
        case TOK_SHL:
            val = cval_shl(s->ctx, val, val2);
            break;
        case TOK_SAR:
            val = cval_shl(s->ctx, val, cval_neg(s->ctx, val2));
            break;
        case '<':
            val = cval_cmp_lt(s->ctx, val, val2);
            break;
        case '>':
            val = cval_cmp_lt(s->ctx, val2, val);
            break;
        case TOK_LTE:
            val = cval_cmp_le(s->ctx, val, val2);
            break;
        case TOK_GTE:
            val = cval_cmp_le(s->ctx, val2, val);
            break;
        case TOK_EQ:
            val = cval_cmp_eq(s->ctx, val, val2);
            break;
        case TOK_NEQ:
            val = cval_cmp_neq(s->ctx, val, val2);
            break;
        case '&':
            val = cval_and(s->ctx, val, val2);
            break;
        case TOK_XOR:
            val = cval_xor(s->ctx, val, val2);
            break;
        case '|':
            val = cval_or(s->ctx, val, val2);
            break;
        default:
            abort();
        }
        check_error(s, val);
    }
    return val;
}

BCValue eval_expr(ParseState *s, int flags)
{
    return eval_expr_binary(s, flags, 9);
}

/* return an exception in case of error, except for 'true' runtime
   errors such as division by zero. */
BCValue eval_formula(BCContext *ctx, int *pshow_result_flag, const char *expr)
{
    ParseState s_s, *s = &s_s;
    BCValue val;
    BOOL show_result_flag;
    
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->buf_ptr = expr;
    show_result_flag = TRUE;

    if (setjmp(s->jmp_env)) {
        return BC_EXCEPTION;
    }

    next_token(s);
    val = cnull_new(ctx);
    for(;;) {
        cval_free(ctx, val);
        val = eval_expr(s, 0);
        if (s->token.val == TOK_EOF) {
            break;
        } else if (s->token.val == ';') {
            
            while (s->token.val == ';') {
                next_token(s);
            }
            if (s->token.val == TOK_EOF) {
                show_result_flag = FALSE;
                break;
            }
        } else {
            cval_free(ctx, val);
            eval_error(s, "invalid characters at the end");
        }
    }
    *pshow_result_flag = show_result_flag;
    return val;
}

/*************************************************/
/* test */

#define TM_JS  (1 << 0)
#define TM_HEX (1 << 1)

static void bc_test2(const char *input, const char *expected, int flags)
{
    BCContext *ctx;
    BCValue val;
    BOOL show_result;
    char *result;
    
    ctx = bc_context_new();
    ctx->hex_output = (flags & TM_HEX) != 0;
    ctx->js_mode = (flags & TM_JS) != 0;
    ctx->tensor_output_lf = FALSE;

    val = eval_formula(ctx, &show_result, input);
    assert(show_result);

    if (cval_is_error(val)) {
        char buf[256];
        bc_get_error(ctx, buf, sizeof(buf));
        fprintf(stderr, "input='%s'\n", input);
        fprintf(stderr, "%s\n", buf);
        exit(1);
    } else {
        DynBuf dbuf;
        dbuf_init(&dbuf);
        cval_toString(ctx, &dbuf, val);
        dbuf_putc(&dbuf, '\0');
        result = (char *)dbuf.buf;
        if (strcmp(result, expected) != 0) {
            fprintf(stderr, "ERROR: input='%s' result='%s' expected='%s'\n",
                    input, result, expected);
            free(result);
            exit(1);
        }
        free(result);
    }
    cval_free(ctx, val);
    
    bc_context_free(ctx);
}

static void bc_test(const char *input, const char *expected)
{
    return bc_test2(input, expected, 0);
}

void bfcalc_test(void)
{
    /* integer */
    bc_test("1+2*3", "7");
    bc_test("1 << 31", "2147483648");
    bc_test("1 << 32", "4294967296");
    bc_test("(-3) % 2", "1");
    bc_test("3 % (-2)", "1");
    bc_test("1 == 1", "true");
    bc_test("1 == 2", "false");
    bc_test("1 != 2", "true");
    bc_test("1 < 2", "true");
    bc_test("1 > 2", "false");
    bc_test("1 <= 2", "true");
    bc_test("1 <= 1", "true");
    bc_test("1 >= 2", "false");
    bc_test("1 >= 1", "true");
    bc_test("1 + true", "2");
    bc_test("0xaa | 0x55", "255");
    bc_test("3 & 1", "1");
    bc_test("3 ^^ 1", "2");
    bc_test("divrem(10,3)", "Array(3, 1)");
    bc_test("divrem(-10,3)", "Array(-4, 2)");
    bc_test("fact(10)", "3628800");
    bc_test("comb(100,30)", "29372339821610944823963760");
    bc_test("comb(100,80)", "535983370403809682970");
    bc_test("invmod(3,101)", "34");
    bc_test("invmod(16,101)", "19");
    bc_test("pmod(123,1234567,618970019642690137449562111)", "184445118581190103495826148");
    bc_test("pmod(123,-41234,618970019642690137449562111)", "190069040174992308417613731");
    bc_test("pmod(123,0,13)", "1");
    bc_test("ilog2(1023)", "9");
    bc_test("ilog2(1024)", "10");
    bc_test("ilog2(0)", "-1");
    bc_test("ctz(0)", "-1");
    bc_test("ctz(1024)", "10");
    bc_test("ctz(1022)", "1");
    bc_test("isprime(961)", "false");
    bc_test("isprime(1021)", "true");
    bc_test("isprime(2^107-1)", "true");
    bc_test("isprime((2^107-1)*(2^89-1))", "false");
    bc_test("nextprime(2^89)", "618970019642690137449562141");
    bc_test("factor((2^89-1)*2^3*11*13^2*1009)", "Array(2, 2, 2, 11, 13, 13, 1009, 618970019642690137449562111)");
    bc_test("factor(1)", "Array()");
    
    /* fraction */
    bc_test("Fraction(5, 2)", "5//2");
    bc_test("1//3 + 1", "4//3");
    bc_test("1//3 + true", "4//3");
    bc_test("(3//5)^10", "59049//9765625");
    bc_test("trunc(5//2)", "2");
    bc_test("floor(5//2)", "2");
    bc_test("ceil(5//2)", "3");
    bc_test("round(5//2)", "3");
    bc_test("-2//3%1//5", "2//15");
    bc_test("-2//3 / 1//5", "-10//3");
    bc_test("int(5//2)", "2");
    bc_test("num(5//2)", "5");
    bc_test("den(5//2)", "2");
    bc_test("bestappr(PI,1000)", "355//113");
    bc_test("bestappr(exp(1.0),1000)", "1457//536");
    bc_test("bestappr(exp(1.0l),1000)", "1457//536");
    
    /* decimal */
    bc_test("Decimal(2)", "2.0");
    bc_test("1/4", "0.25");
    bc_test("0.1+0.2", "0.3");
    bc_test("0.1+true", "1.1");
    bc_test("3.0^10", "59049.0");
    bc_test("int(2.5)", "2");
    bc_test("trunc(2.5)", "2.0");
    bc_test("trunc(-2.5)", "-2.0");
    bc_test("floor(2.5)", "2.0");
    bc_test("floor(-2.5)", "-3.0");
    bc_test("ceil(2.5)", "3.0");
    bc_test("ceil(-2.5)", "-2.0");
    bc_test("round(2.5)", "3.0");
    bc_test("round(2.4)", "2.0");
    bc_test("round(2.6)", "3.0");
    bc_test("sqrt(2)", "1.414213562373095");
    bc_test("exp(1.1)", "3.004166023946433");
    bc_test("log(3.1)", "1.131402111491101");
    bc_test("log(-10)", "2.302585092994046-3.141592653589793i");
    bc_test("log2(5)", "2.321928094887361");
    bc_test("log10(5)", "0.6989700043360186");
    bc_test("sin(1.1)", "0.8912073600614353");
    bc_test("cos(1.1)", "0.4535961214255774");
    bc_test("tan(1.1)", "1.964759657248652");
    bc_test("atan2(2.1,1.1)", "1.08828303177242");
    bc_test("2.1^3.1", "9.974239992658708");
    bc_test("2^-2", "0.25");
    bc_test("PI", "3.141592653589793");
    bc_test("1/0", "Inf");
    bc_test("0/0", "NaN");
    bc_test("1.0 < 2.0", "true");
    bc_test("NaN < NaN", "false");
    bc_test("NaN == NaN", "false");
    bc_test("-Inf < Inf", "true");
    bc_test("123_456.7_89e-0_1", "12345.6789");
    
    bc_test("sinc(0)", "1.0");
    bc_test("sinc(0.5)", "0.6366197723675816");
    bc_test("todb(2)", "3.010299956639812");
    bc_test("fromdb(16)", "39.81071705534976");
    bc_test("todeg(PI)", "180.0");
    bc_test("fromdeg(180)", "3.141592653589792");

    bc_test("sinh(1)", "1.175201193643802");
    bc_test("cosh(1)", "1.543080634815244");
    bc_test("tanh(1)", "0.7615941559557649");
    bc_test("asinh(1)", "0.881373587019543");
    bc_test("acosh(2)", "1.316957896924817");
    bc_test("atanh(0.5)", "0.549306144334055");

    /* float */
    bc_test("Float(2)", "2.0l");
    bc_test("0.1l+0.2l", "0.30000000000000004l");
    bc_test("0.1l+true", "1.1l");
    bc_test("int(2.5l)", "2");
    bc_test("trunc(2.5l)", "2.0l");
    bc_test("trunc(-2.5l)", "-2.0l");
    bc_test("floor(2.5l)", "2.0l");
    bc_test("floor(-2.5l)", "-3.0l");
    bc_test("ceil(2.5l)", "3.0l");
    bc_test("ceil(-2.5l)", "-2.0l");
    bc_test("round(2.5l)", "3.0l");
    bc_test("round(2.4l)", "2.0l");
    bc_test("round(2.6l)", "3.0l");
    bc_test("sqrt(2.0l)", "1.4142135623730951l");

    bc_test("exp(1.0l)", "2.718281828459045l");
    bc_test("log(3.0l)", "1.0986122886681098l");
    bc_test("sin(1.0l)", "0.8414709848078965l");
    bc_test("cos(1.0l)", "0.5403023058681398l");
    bc_test("tan(1.0l)", "1.5574077246549023l");
    bc_test("atan2(2.0l,1.0l)", "1.1071487177940904l");
    bc_test("2.1l^3.1l", "9.97423999265871l");

    /* complex */
    bc_test("Complex(2,3.0)", "2.0+3.0i");
    bc_test("a=(2-3*I);sqrt(a*a)", "2.0-3.0i");
    bc_test("exp(2.1+I*1.1)","3.704142999242933+7.277750729592537i");
    bc_test("log(1+2*I)", "0.8047189562170503+1.107148717794091i");
    bc_test("arg(1.1-2.1*I)", "-1.08828303177242");
    bc_test("(1+I)^(2+I)", "-0.3097435049284941+0.8576580125887354i");
    bc_test("(1+2*I)^10", "237-3116i");

    bc_test("exp(2.0l+I*1.0l)","3.992324048441272l+6.217676312367968li");
    bc_test("arg(1.0l-2.0li)", "-1.1071487177940904l");
    bc_test("re(1)", "1");
    bc_test("im(1)", "0");
    bc_test("sin(1+2*I)", "3.165778513216166+1.959601041421604i");
    bc_test("cos(1+2*I)", "2.032723007019664-3.051897799151798i");
    bc_test("tan(1+2*I)", "0.0338128260798969+1.014793616146633i");
    bc_test("0i == 0", "true");
    bc_test("1i == 0", "false");
    bc_test("1 + 0i == 0", "false");
    bc_test("divrem(4+3*I,1+I)", "Array(4-1i, -1+0i)");
    bc_test("(4+3*I)//(1+I)","7//2-1//2i");
    bc_test("(1//1+1i)/(1+10i)", "11//101-9//101i");
    /* polynomial */
    bc_test("Polynomial(2)", "Polynomial(2)");
    bc_test("Polynomial([1,2,3.0])", "3.0*X^2+2.0*X+1.0");
    bc_test("-(1-X)^2", "-X^2+2*X-1");
    bc_test("X == X", "true");
    bc_test("X == X*0", "false");
    bc_test("deg(X^2+1)", "2");
    bc_test("deg(0*X)", "-1");
    bc_test("r=1+X;q=1+X+X^2;b=X^2-1;a=q*b+r;a%b", "X+1");
    bc_test("r=1+X;q=1+X+X^2;b=X^2-1;a=q*b+r;divrem(a, b)", "Array(X^2+X+1, X+1)");
    bc_test("(X+2)/5", "0.2*X+0.4");
    bc_test("(X+2//1)/5", "1//5*X+2//5");
    bc_test("(1+X+X^2)(2)", "7");
    bc_test("gcd((X-1)*(X-2),(X-1)*(X-3))", "X-1");
    bc_test("deriv(1-2*X^2+2*X^3)", "6*X^2-4*X");
    bc_test("integ(6*X^2-4*X)", "2.0*X^3-2.0*X^2");
    bc_test("(1+2*X+3*X^2)[1]", "2");
    bc_test("(1+2*X+3*X^2)[-1]", "3");
    bc_test("polroots((X-1)*(X-2)*(X-3)*(X-4)*(X-0.1))", "[0.1+0.0i, 0.9999999999999997+0.0i, 2.000000000000009+0.0i, 2.999999999999985+0.0i, 4.000000000000006-0.0i]");
    
    /* rational function */
    bc_test("RationalFunction(X,1+X)", "(X)//(X+1)");
    bc_test("(2*X-2)//(X^2-1)", "(Polynomial(2))//(X+1)");
    bc_test("((X)//(X^2+1))(2//1)", "2//5");
    bc_test("deriv((X^2-X+1)//(X-1))", "(X^2-2*X)//(X^2-2*X+1)");
    bc_test("num((1+X)//X)", "X+1");
    bc_test("den((1+X)//X)", "X");

    /* series */
    bc_test("O(1+X^2)", "O(X^2)");
    bc_test("O(1//X^2)", "O(X^-2)");
    bc_test("-(X-X^2+O(X^3))", "-X+X^2+O(X^3)");
    bc_test("(X+O(X^3))+(-X^2+X^3)", "X-X^2+O(X^3)");
    bc_test("(X+O(X^3))+(1+O(X^2))", "1+X+O(X^2)");
    bc_test("O(X^5)+X//(1+X)", "1.0*X-1.0*X^2+1.0*X^3-1.0*X^4+O(X^5)");
    bc_test("1//(1+X+O(X^3))", "1//1-1//1*X+1//1*X^2+O(X^3)");
    bc_test("(X+X^2+X^3+X^4+O(X^5))(0.1)","0.1111");
    bc_test("deriv(1//X+1-X+X^2-X^3+X^4+O(X^5))", "-1.0*X^-2-1.0+2.0*X-3.0*X^2+4.0*X^3+O(X^4)");
    bc_test("integ(1-X+X^2-X^3+X^4+O(X^5))", "1.0*X-0.5*X^2+0.3333333333333333*X^3-0.25*X^4+0.2*X^5+O(X^6)");
    bc_test("log(1+X+O(X^5))", "1.0*X-0.5*X^2+0.3333333333333333*X^3-0.25*X^4+O(X^5)");
    bc_test("log(1+X+O(X^5)+0//1)", "1//1*X-1//2*X^2+1//3*X^3-1//4*X^4+O(X^5)");
    bc_test("exp(3*X^2+O(X^10))", "1.0+3.0*X^2+4.5*X^4+4.5*X^6+3.375*X^8+O(X^10)");
    bc_test("exp(1+3*X+O(X^3))","2.718281828459045+8.154845485377135*X+12.2322682280657*X^2+O(X^3)");
    bc_test("(X+X^2+O(X^5))^3", "X^3+3*X^4+3*X^5+X^6+O(X^7)");
    bc_test("(X+X^2+O(X^5))^0", "1+O(X^4)");
    bc_test("(1+X+O(X^6))^(2+X)", "1.0+2.0*X+2.0*X^2+1.500000000000001*X^3+0.8333333333333333*X^4+0.4166666666666666*X^5+O(X^6)");
    bc_test("sin(X+O(X^6))", "1.0*X-0.1666666666666667*X^3+0.008333333333333333*X^5+O(X^6)");
    bc_test("sin(I*X+O(X^4))", "(1.0i)*X+(0.1666666666666667i)*X^3+O(X^4)");
    bc_test("sin(X+O(X^6)+0//1)", "1//1*X-1//6*X^3+1//120*X^5+O(X^6)");
    bc_test("cos(X+O(X^6))", "1.0-0.5*X^2+0.04166666666666667*X^4+O(X^6)");
    bc_test("tan(X+O(X^6))", "1.0*X+0.3333333333333333*X^3+0.1333333333333333*X^5+O(X^6)");
    bc_test("tan(X+O(X^6)+0//1)", "1//1*X+1//3*X^3+2//15*X^5+O(X^6)");
    bc_test("(1//(X^2*(2+X^2))+O(X^6))[4]", "-0.0625");
    bc_test("(1//(X^2*(2+X^2))+O(X^6))[-2]", "0.5");
    bc_test("(1//(X^2*(2+X^2))+O(X^6))[10]", "0.0");
    
    /* tensor */
    bc_test("Tensor(3)", "Tensor(3)");
    bc_test("shape([1, -2])", "Array(2)");
    bc_test("[1, 2.0, 1/5]", "[1.0, 2.0, 0.2]");
    bc_test("abs([1, -2])", "[1, 2]");
    bc_test("abs([3 + 4*I, -2])", "[5.0, 2.0]");
    bc_test("sqrt([1, 2, -2])", "[1.0, 1.414213562373095, NaN]");
    bc_test("[1,2]+[2,3]", "[3, 5]");
    bc_test("[1,2].*[2,3]", "[2, 6]");
    bc_test("[1,2]/[2,3]", "[0.5, 0.6666666666666667]");
    bc_test("[7,5]%[2,3]", "[1, 2]");
    bc_test("[7,5].*[2,3]", "[14, 15]");
    bc_test("typeof([1,2]/[2,3])", "\"Tensor(Decimal)\"");
    bc_test("[1.0,2]^[2,3]", "[1.0, 8.0]");
    bc_test("[1.0,2]-[2,3]", "[-1.0, -1.0]");
    bc_test("[X+1,1,1.0]", "[1.0*X+1.0, Polynomial(1.0), Polynomial(1.0)]");
    
    /* tensor: slice */
    bc_test("a=[[1,2,3],[4,5,6]];a[1,2]", "6");
    bc_test("a=[[1,2,3],[4,5,6]];a[1]", "[4, 5, 6]");
    bc_test("a=[[1,2,3],[4,5,6]];a[1,1:2]", "[5]");
    bc_test("a=[[1,2,3],[4,5,6]];a[:,1]", "[2, 5]");
    bc_test("a=[[1,2,3],[4,5,6]];a[0:2,1:3]", "[[2, 3], [5, 6]]");
    bc_test("a=[1,2,3];a[1:3]=[4,5];a", "[1, 4, 5]");
    bc_test("a=[1,2,3];a[1]=4;a", "[1, 4, 3]");
    bc_test("a=[[1,2,3],[4,5,6]];a[0:2,1:3]=[[1,2],[3,4]];a", "[[1, 1, 2], [4, 3, 4]]");
    bc_test("a=Tensor(3);a[]", "3");
    bc_test("a=Tensor(3);a[]=4;a", "Tensor(4)");
    
    /* tensor: broadcast */
    bc_test("[1,2]+[[2],[3]]", "[[3, 4], [4, 5]]");
    bc_test("[[1,2],[3,4]]+[-1,1]", "[[0, 3], [2, 5]]");
    
    /* tensor: matrix */
    bc_test("trace([[1,2],[3,4]])", "5");
    bc_test("[[1,2],[3,4]]*[[3],[4]]", "[[11], [25]]");
    bc_test("[[1,2],[3,4]]^3", "[[37, 54], [81, 118]]");
    bc_test("[ [[1,2],[3,4]], [[-1,2],[3,4]] ] * [[3],[4]]", "[[[11], [25]], [[5], [25]]]");
    bc_test("trans([[1,2,3],[4,5,6]])", "[[1, 4], [2, 5], [3, 6]]");
    bc_test("trans([1,2,3])", "[[1], [2], [3]]");
    bc_test("inverse([[1,2],[3,4]])", "[[-2.0, 1.0], [1.5, -0.5]]");
    bc_test("[[1+I,2],[3,4]]^-3", "[[1.286+0.302i, -0.548-0.236i], [-0.822-0.354i, 0.346+0.222i]]");
    bc_test("conj([[1+I,2,3-I]])","[[1-1i, 2-0i, 3+1i]]");
    bc_test("re([[1+I,2,3-I]])","[[1, 2, 3]]");
    bc_test("im([[1+I,2,3-I]])","[[1, 0, -1]]");
    bc_test("charpoly(mathilbert(4))","1//1*X^4-176//105*X^3+3341//12600*X^2-41//23625*X+1//6048000");
    bc_test("charpoly([[1,2],[3,4]])", "X^2-5*X-2");
    bc_test("eigenvals([[1,2],[3,4]])", "[-0.3722813232690143+0.0i, 5.372281323269014-0.0i]");
    bc_test("dp([1, 2, 3], [3, -4, -7])", "-26");
    bc_test("cp([1, 2, 3], [3, -4, -7])", "[-2, 16, -10]");
    bc_test("det(mathilbert(4))", "1//6048000");
    bc_test("rank([[1,2,1],[-2,-3,1],[3,5,0]])", "2");
    bc_test("ker([[1,2,1],[-2,-3,1],[3,5,0]])", "[[5//1], [-3//1], [1//1]]");

    /* array */
    bc_test2("a=[1, 2, 3, 4];a[2]", "3", TM_JS);
    bc_test2("a=[1, 2, 3, 4];a[1:3]", "[2, 3]", TM_JS);
    bc_test2("a=[1, 2, 3, 4];a[1:]", "[2, 3, 4]", TM_JS);
    bc_test2("a=[1, 2, 3, null];a[:]", "[1, 2, 3, null]", TM_JS);
    bc_test2("len([1,2,3])", "3", TM_JS);

    /* string */
    bc_test("len('ab€d')", "4");
    bc_test("\"a€cd\"[1]","\"€\"");
    bc_test("\"a€cd\"[1:3]","\"€c\"");
    bc_test("\"a€cd\"[:-1]","\"a€c\"");
    bc_test("\"a€cd\"+\"efg\"","\"a€cdefg\"");
    bc_test("chr(0x20ac)", "\"€\"");
    bc_test("ord(\"€\")", "8364");
    
    /* misc */
    bc_test("convert(1, \"c\", \"km/s\")", "299792.458");
    bc_test("convert(100, \"°C\", \"°F\")", "211.9999999999999");
    bc_test("convert(100, \"square feet\", \"m^2\")", "9.290304000000001");
    bc_test("convert(20000, \"m²\", \"ha\")", "2.0");
}

/*************************************************/
/* repl */

static ReadlineState readline_state;
static uint8_t readline_cmd_buf[256];
static uint8_t readline_kill_buf[256];
static char readline_history[256];

#define STYLE_DEFAULT    COLOR_BRIGHT_GREEN
#define STYLE_COMMENT    COLOR_WHITE
#define STYLE_STRING     COLOR_BRIGHT_CYAN
#define STYLE_REGEX      COLOR_CYAN
#define STYLE_NUMBER     COLOR_GREEN
#define STYLE_KEYWORD    COLOR_BRIGHT_WHITE
#define STYLE_FUNCTION   COLOR_BRIGHT_YELLOW
#define STYLE_TYPE       COLOR_BRIGHT_MAGENTA
#define STYLE_IDENTIFIER COLOR_BRIGHT_GREEN
#define STYLE_ERROR      COLOR_RED
#define STYLE_RESULT     COLOR_BRIGHT_WHITE
#define STYLE_ERROR_MSG  COLOR_BRIGHT_RED

void readline_find_completion(const char *cmdline)
{
}

static BOOL is_word(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        c == '_' || c == '$';
}

/* return the color for the character at position 'pos' and the number
   of characters of the same color */
static int term_get_color(int *plen, const char *buf, int pos, int buf_len)
{
    int c, color, pos1, len;

    c = buf[pos];
    if (c == '"' || c == '\'') {
        pos1 = pos + 1;
        for(;;) {
            if (buf[pos1] == '\0' || buf[pos1] == c)
                break;
            if (buf[pos1] == '\\' && buf[pos1 + 1] != '\0')
                pos1 += 2;
            else
                pos1++;
        }
        if (buf[pos1] != '\0')
            pos1++;
        len = pos1 - pos;
        color = STYLE_STRING;
    } else if (c == '/' && buf[pos + 1] == '*') {
        pos1 = pos + 2;
        while (buf[pos1] != '\0' &&
               !(buf[pos1] == '*' && buf[pos1 + 1] == '/')) {
            pos1++;
        }
        if (buf[pos1] != '\0')
            pos1 += 2;
        len = pos1 - pos;
        color = STYLE_COMMENT;
    } else if ((c >= '0' && c <= '9') || c == '.') {
        pos1 = pos + 1;
        while (is_word(buf[pos1]))
            pos1++;
        len = pos1 - pos;
        color = STYLE_NUMBER;
    } else if (is_word(c)) {
        pos1 = pos + 1;
        while (is_word(buf[pos1]))
            pos1++;
        len = pos1 - pos;
        while (buf[pos1] == ' ')
            pos1++;
        if (buf[pos1] == '(') {
            color = STYLE_FUNCTION;
        } else {
            color = STYLE_IDENTIFIER;
        }
    } else {
        color = STYLE_DEFAULT;
        len = 1;
    }
    *plen = len;
    return color;
}

static int eval_buf(BCContext *ctx, const char *cmd)
{
    BCValue val;
    BOOL show_result;
    int ret;
    
    val = eval_formula(ctx, &show_result, cmd);
    if (cval_is_error(val)) {
        char buf[256];
        bc_get_error(ctx, buf, sizeof(buf));
        fprintf(stderr, "%s%s\n%s", term_colors[STYLE_ERROR_MSG], buf, term_colors[COLOR_NONE]);
        ret = 1;
    } else {
        DynBuf dbuf;
        /* save the last result to '_' */
        set_variable(ctx, "_", val, FALSE, FALSE);
        
        if (show_result) {
            dbuf_init(&dbuf);
            cval_toString(ctx, &dbuf, val);
            dbuf_putc(&dbuf, '\0');
            printf("%s%s\n%s", term_colors[STYLE_RESULT], (char *)dbuf.buf, term_colors[COLOR_NONE]);
            dbuf_free(&dbuf);
        }
        ret = 0;
    }
    cval_free(ctx, val);
    return ret;
}

static void help_directive(void)
{
    printf("\\h          this help\n"
           "\\x          hexadecimal number display\n"
           "\\d          decimal number display\n"
           "\\p [m [e]]  set the decimal float precision to 'm' digits and 'e' exponent bits\n"
           "\\bp [m [e]] set the binary float precision to 'm' bits and 'e' exponent bits\n"
           "\\js         toggle Javascript mode ('^' is xor instead of power, [] is an array literal instead of tensor)\n"
           );
}

static int get_word(char *buf, size_t buf_size, const char **pp)
{
    const char *p = *pp;
    char *q;
    
    q = buf;
    while (*p != ' ' && *p != '\0') {
        if ((q - buf) >= buf_size - 1)
            return -1;
        *q++ = *p++;
    }
    *q = '\0';
    while (*p == ' ')
        p++;
    *pp = p;
    return 0;
}

/* return true if a command can be issued after it */
static BOOL handle_directive(BCContext *ctx, const char **pp)
{
    const char *p = *pp;
    int ret;
    char cmd[64], buf[128];

    if (get_word(cmd, sizeof(cmd), &p))
        goto fail;
    ret = FALSE;
    if (!strcmp(cmd, "h")) {
        help_directive();
    } else if (!strcmp(cmd, "d")) {
        ctx->hex_output = FALSE;
        ret = TRUE;
    } else if (!strcmp(cmd, "x")) {
        ctx->hex_output = TRUE;
        ret = TRUE;
    } else if (!strcmp(cmd, "p")) {
        int prec, exp_bits;
        if (*p != '\0') {
            if (get_word(buf, sizeof(buf), &p))
                goto fail;
            if (!strcmp(buf, "d64")) {
                prec = 16;
                exp_bits = 10;
            } else if (!strcmp(buf, "d128")) {
                prec = 34;
                exp_bits = 14;
            } else {
                prec = strtoul(buf, NULL, 0);
                if (prec < BF_PREC_MIN || prec > BF_PREC_MAX) {
                    printf("invalid precision\n");
                    goto done;
                }
                while (*p == ' ')
                    p++;
                exp_bits = BF_EXP_BITS_MAX;
                if (*p != '\0') {
                    exp_bits = strtoul(p, (char **)&p, 0);
                    if (exp_bits < BF_EXP_BITS_MIN || prec > BF_EXP_BITS_MAX) {
                        printf("invalid number of exponent bits\n");
                        goto done;
                    }
                }
            }
            ctx->dec_prec = prec;
            ctx->dec_flags = bf_set_exp_bits(exp_bits) | BF_RNDN | BF_FLAG_SUBNORMAL;
        }
        printf("decimal precision: %d digits, %d exponent bits\n",
               ctx->dec_prec, bf_get_exp_bits(ctx->dec_flags));
    } else if (!strcmp(cmd, "bp")) {
        int prec, exp_bits;
        if (*p != '\0') {
            if (get_word(buf, sizeof(buf), &p))
                goto fail;
            if (!strcmp(buf, "f16")) {
                prec = 11;
                exp_bits = 5;
            } else if (!strcmp(buf, "f32")) {
                prec = 24;
                exp_bits = 8;
            } else if (!strcmp(buf, "f64")) {
                prec = 53;
                exp_bits = 11;
            } else if (!strcmp(buf, "f128")) {
                prec = 113;
                exp_bits = 15;
            } else {
                prec = strtoul(buf, NULL, 0);
                if (prec < BF_PREC_MIN || prec > BF_PREC_MAX) {
                    printf("invalid precision\n");
                    goto done;
                }
                while (*p == ' ')
                    p++;
                exp_bits = BF_EXP_BITS_MAX;
                if (*p != '\0') {
                    exp_bits = strtoul(p, (char **)&p, 0);
                    if (exp_bits < BF_EXP_BITS_MIN || prec > BF_EXP_BITS_MAX) {
                        printf("invalid number of exponent bits\n");
                        goto done;
                    }
                }
            }
            ctx->float_prec = prec;
            ctx->float_flags = bf_set_exp_bits(exp_bits) | BF_RNDN | BF_FLAG_SUBNORMAL;
        }
        printf("float precision: %d digits (~%d digits), %d exponent bits\n",
               ctx->float_prec, (ctx->float_prec * 77) >> 8,
               bf_get_exp_bits(ctx->float_flags));
    } else if (!strcmp(cmd, "js")) {
        ctx->js_mode ^= 1;
        printf("js mode %s\n", ctx->js_mode ? "enabled" : "disabled");
    } else {
    fail:
        printf("Unknown directive: %s - use \\h for help\n", cmd);
    }
done:
    *pp = p;
    return ret;
}

static void repl_run(BCContext *ctx)
{
    ReadlineState *s = &readline_state;
    const char *cmd, *p;

    s->term_width = readline_tty_init();
    s->term_cmd_buf = readline_cmd_buf;
    s->term_kill_buf = readline_kill_buf;
    s->term_cmd_buf_size = sizeof(readline_cmd_buf);
    s->term_history = readline_history;
    s->term_history_buf_size = sizeof(readline_history);
    s->get_color = term_get_color;
    
    for(;;) {
        cmd = readline_tty(s, "bfcalc > ", FALSE);
        if (!cmd)
            break;
        p = cmd;
        if (p[0] == '\\' && p[1] != '\0') {
            p++;
            if (!handle_directive(ctx, &p))
                continue;
        }
        if (*p == '\0')
            continue;
        eval_buf(ctx, p);
    }
}

static void help(void)
{
    printf("usage: bfcalc [options]\n"
           "-h        help\n"
           "-H        hex display\n"
           "-j        use JS operator syntax ('^' is xor instead of power)\n"
           "-t        built-in autotest\n"
           "-e expr   eval the expression 'expr\n");
    exit(1);
}

int main(int argc, char **argv)
{
    BCContext *ctx;
    const char *opt;
    int arg_idx, ret, js_mode = 0, hex_mode = 0, test_mode = 0;
    const char *expr = NULL;
    
    arg_idx = 1;
    while (arg_idx < argc) {
        opt = argv[arg_idx++];
        if (!strcmp(opt, "-h")) {
            help();
        } else if (!strcmp(opt, "-j")) {
            js_mode = 1;
            arg_idx++;
        } else if (!strcmp(opt, "-H")) {
            hex_mode = 1;
            arg_idx++;
        } else if (!strcmp(opt, "-t")) {
            test_mode = 1;
            arg_idx++;
        } else if (!strcmp(opt, "-e")) {
            if (arg_idx < argc) {
                expr = argv[arg_idx++];
                break;
            }
            fprintf(stderr, "missing expression for -e\n");
            exit(1);
        } else{
            break;
        }
    }

    if (test_mode) {
        bfcalc_test();
        return 0;
    }
    
    ctx = bc_context_new();
    ctx->hex_output = hex_mode;
    ctx->js_mode = js_mode;

    if (expr) {
        ret = eval_buf(ctx, expr);
    } else {
        repl_run(ctx);
        ret = 0;
    }
    
    bc_context_free(ctx);
    return ret;
}
