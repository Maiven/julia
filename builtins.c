/*
  implementations of some built-in functions and utilities
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#ifndef NO_BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"

// --- exception raising ---

extern char *julia_home;
extern jmp_buf ExceptionHandler;
extern jmp_buf *CurrentExceptionHandler;

void jl_error(const char *str)
{
    ios_printf(ios_stderr, "%s\n", str);
    longjmp(*CurrentExceptionHandler, 1);
}

void jl_errorf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ios_vprintf(ios_stderr, fmt, args);
    ios_printf(ios_stderr, "\n");
    va_end(args);
    longjmp(*CurrentExceptionHandler, 1);
}

void jl_too_few_args(const char *fname, int min)
{
    jl_errorf("%s: too few arguments (expected %d)", fname, min);
}

void jl_too_many_args(const char *fname, int max)
{
    jl_errorf("%s: too many arguments (expected %d)", fname, max);
}

void jl_type_error(const char *fname, const char *expected, jl_value_t *got)
{
    jl_errorf("type error: %s: expected %s, got %s",
              fname, expected, jl_tname((jl_value_t*)jl_typeof(got))->name->name);
}

/*
  equivalent julia code:
  expr(head, args...) = Expr.new(head, args)
*/
jl_expr_t *jl_expr(jl_sym_t *head, size_t n, ...)
{
    va_list args;
    size_t i;
    jl_expr_t *ex = jl_exprn(head,n);
    va_start(args, n);
    for(i=0; i < n; i++) {
        jl_tupleset(ex->args, i, va_arg(args, jl_value_t*));
    }
    va_end(args);
    return ex;
}

jl_expr_t *jl_exprn(jl_sym_t *head, size_t n)
{
    jl_value_t *ctor_args[2];
    ctor_args[0] = (jl_value_t*)head;
    ctor_args[1] = (jl_value_t*)jl_alloc_tuple(n);
    return (jl_expr_t*)jl_apply(jl_expr_type->fnew, ctor_args, 2);
}

JL_CALLABLE(jl_f_is)
{
    JL_NARGS(is, 2, 2);
    if (args[0] == args[1])
        return jl_true;
    return jl_false;
}

JL_CALLABLE(jl_f_identity)
{
    JL_NARGS(identity, 1, 1);
    return args[0];
}

JL_CALLABLE(jl_f_typeof)
{
    JL_NARGS(typeof, 1, 1);
    return jl_full_type(args[0]);
}

JL_CALLABLE(jl_f_subtype)
{
    JL_NARGS(subtype, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(subtype, type, args[1]);
    return (jl_subtype(args[0],args[1],0,0) ? jl_true : jl_false);
}

JL_CALLABLE(jl_f_istype)
{
    JL_NARGS(istype, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(istype, type, args[1]);
    if (jl_is_tuple(args[0]))
        return (jl_subtype(args[0],args[1],1,0) ? jl_true : jl_false);
    return (jl_subtype((jl_value_t*)jl_typeof(args[0]),args[1],0,0)
            ? jl_true : jl_false);
}

JL_CALLABLE(jl_f_typeassert)
{
    JL_NARGS(typeassert, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(typeassert, type, args[1]);
    int ok = jl_is_tuple(args[0]) ?
        jl_subtype(args[0],args[1],1,0) :
        jl_subtype((jl_value_t*)jl_typeof(args[0]),args[1],0,0);
    if (!ok)
        jl_error("type assertion failed");
    return args[0];
}

static jl_value_t *jl_arrayref(jl_array_t *a, size_t i);

JL_CALLABLE(jl_f_apply)
{
    JL_NARGSV(apply, 1);
    JL_TYPECHK(apply, function, args[0]);
    size_t n=0, i, j;
    for(i=1; i < nargs; i++) {
        JL_TYPECHK(apply, tuple, args[i]);
        n += ((jl_tuple_t*)args[i])->length;
    }
    jl_value_t **newargs = alloca(n * sizeof(jl_value_t*));
    n = 0;
    for(i=1; i < nargs; i++) {
        jl_tuple_t *t = (jl_tuple_t*)args[i];
        for(j=0; j < t->length; j++)
            newargs[n++] = jl_tupleref(t, j);
    }
    return jl_apply((jl_function_t*)args[0], newargs, n);
}

JL_CALLABLE(jl_f_error)
{
    JL_NARGS(error, 1, 1);
    if (jl_typeof(args[0]) == jl_array_uint8_type) {
        jl_error((char*)((jl_array_t*)args[0])->data);
    }
    else {
        jl_error("error: expected string");
    }
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_time_thunk)
{
    JL_NARGS(time_thunk, 1, 1);
    JL_TYPECHK(time_thunk, function, args[0]);
    double t0 = clock_now();
    jl_value_t *result = jl_apply((jl_function_t*)args[0], NULL, 0);
    double t1 = clock_now();
    ios_printf(ios_stdout, "elapsed time: %.4f sec\n", t1-t0);
    return result;
}

int asprintf(char **strp, const char *fmt, ...);

void jl_load(const char *fname)
{
    char *fpath = (char*)fname;
    if (julia_home)
        asprintf(&fpath, "%s/%s", julia_home, fname);
    jl_value_t *ast = jl_parse_file(fpath);
    if (ast == (jl_value_t*)jl_null)
        jl_errorf("could not open file %s", fpath);
    jl_tuple_t *b = ((jl_expr_t*)ast)->args;
    size_t i, lineno=0;
    jmp_buf *prevh = CurrentExceptionHandler;
    jmp_buf handler;
    CurrentExceptionHandler = &handler;
    if (!setjmp(handler)) {
        for(i=0; i < b->length; i++) {
            // process toplevel form
            jl_value_t *form = jl_tupleref(b, i);
            if (jl_is_expr(form) && ((jl_expr_t*)form)->head == line_sym) {
                lineno = jl_unbox_int32(jl_exprarg(form, 0));
            }
            else {
                //jl_toplevel_eval(form);
                jl_lambda_info_t *lam = (jl_lambda_info_t*)jl_exprarg(form,0);
                (void)jl_interpret_toplevel_expr(lam);
            }
        }
    }
    else {
        CurrentExceptionHandler = prevh;
        ios_printf(ios_stderr, " %s:%d\n", fpath, lineno);
        longjmp(*CurrentExceptionHandler, 1);
    }
    CurrentExceptionHandler = prevh;
    if (fpath != fname) free(fpath);
}

JL_CALLABLE(jl_f_load)
{
    JL_NARGS(load, 1, 1);
    if (jl_typeof(args[0]) == jl_array_uint8_type) {
        char *fname = (char*)((jl_array_t*)args[0])->data;
        jl_load(fname);
    }
    else {
        jl_error("load: expected string");
    }
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_tuple)
{
    size_t i;
    if (nargs == 0) return (jl_value_t*)jl_null;
    jl_tuple_t *t = jl_alloc_tuple(nargs);
    for(i=0; i < nargs; i++) {
        jl_tupleset(t, i, args[i]);
    }
    return (jl_value_t*)t;
}

JL_CALLABLE(jl_f_tupleref)
{
    JL_NARGS(tupleref, 2, 2);
    JL_TYPECHK(tupleref, tuple, args[0]);
    JL_TYPECHK(tupleref, int32, args[1]);
    jl_tuple_t *t = (jl_tuple_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= t->length)
        jl_error("tupleref: index out of range");
    return jl_tupleref(t, i);
}

JL_CALLABLE(jl_f_tuplelen)
{
    JL_NARGS(tuplelen, 1, 1);
    JL_TYPECHK(tuplelen, tuple, args[0]);
    return jl_box_int32(((jl_tuple_t*)args[0])->length);
}

static size_t field_offset(jl_struct_type_t *t, jl_sym_t *fld)
{
    jl_tuple_t *fn = t->names;
    size_t i;
    for(i=0; i < fn->length; i++) {
        if (jl_tupleref(fn,i) == (jl_value_t*)fld)
            return i;
    }
    jl_errorf("type %s has no field %s", t->name->name->name, fld->name);
    return 0;
}

JL_CALLABLE(jl_f_get_field)
{
    JL_NARGS(getfield, 2, 2);
    JL_TYPECHK(getfield, symbol, args[1]);
    jl_value_t *v = args[0];
    if (!jl_is_struct_type(jl_typeof(v)))
        jl_error("getfield: argument must be a struct");
    size_t i = field_offset((jl_struct_type_t*)jl_typeof(v),
                            (jl_sym_t*)args[1]);
    return ((jl_value_t**)v)[1+i];
}

JL_CALLABLE(jl_f_set_field)
{
    JL_NARGS(setfield, 3, 3);
    JL_TYPECHK(setfield, symbol, args[1]);
    jl_value_t *v = args[0];
    if (!jl_is_struct_type(jl_typeof(v)))
        jl_error("setfield: argument must be a struct");
    jl_struct_type_t *st = (jl_struct_type_t*)jl_typeof(v);
    size_t i = field_offset(st, (jl_sym_t*)args[1]);
    ((jl_value_t**)v)[1+i] = jl_convert(args[2],
                                        (jl_type_t*)jl_tupleref(st->types,i));
    return v;
}

JL_CALLABLE(jl_f_arraylen)
{
    JL_NARGS(arraylen, 1, 1);
    JL_TYPECHK(arraylen, array, args[0]);
    return jl_box_int32(((jl_array_t*)args[0])->length);
}

static jl_value_t *new_scalar(jl_bits_type_t *bt)
{
    size_t nb = bt->nbits/8;
    jl_value_t *v = (jl_value_t*)allocb((NWORDS(LLT_ALIGN(nb,sizeof(void*)))+1)*
                                        sizeof(void*));
    v->type = (jl_type_t*)bt;
    return v;
}

static jl_value_t *jl_arrayref(jl_array_t *a, size_t i)
{
    jl_type_t *el_type = (jl_type_t*)jl_tparam0(jl_typeof(a));
    jl_value_t *elt;
    if (jl_is_bits_type(el_type)) {
        if (el_type == (jl_type_t*)jl_bool_type) {
            if (((int8_t*)a->data)[i] != 0)
                return jl_true;
            return jl_false;
        }
        elt = new_scalar((jl_bits_type_t*)el_type);
        size_t nb = ((jl_bits_type_t*)el_type)->nbits/8;
        switch (nb) {
        case 1:
            *(int8_t*)jl_bits_data(elt)  = ((int8_t*)a->data)[i];  break;
        case 2:
            *(int16_t*)jl_bits_data(elt) = ((int16_t*)a->data)[i]; break;
        case 4:
            *(int32_t*)jl_bits_data(elt) = ((int32_t*)a->data)[i]; break;
        case 8:
            *(int64_t*)jl_bits_data(elt) = ((int64_t*)a->data)[i]; break;
        default:
            memcpy(jl_bits_data(elt), &((char*)a->data)[i*nb], nb);
        }
    }
    else {
        elt = ((jl_value_t**)a->data)[i];
        if (elt == NULL)
            jl_errorf("array[%d]: uninitialized reference error", i+1);
    }
    return elt;
}

JL_CALLABLE(jl_f_arrayref)
{
    JL_NARGS(arrayref, 2, 2);
    JL_TYPECHK(arrayref, array, args[0]);
    JL_TYPECHK(arrayref, int32, args[1]);
    jl_array_t *a = (jl_array_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= a->length)
        jl_errorf("array[%d]: index out of range", i+1);
    return jl_arrayref(a, i);
}

JL_CALLABLE(jl_f_arrayset)
{
    JL_NARGS(arrayset, 3, 3);
    JL_TYPECHK(arrayset, array, args[0]);
    JL_TYPECHK(arrayset, int32, args[1]);
    jl_array_t *b = (jl_array_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= b->length)
        jl_errorf("array[%d]: index out of range", i+1);
    jl_type_t *el_type = (jl_type_t*)jl_tparam0(jl_typeof(b));
    jl_value_t *rhs = jl_convert(args[2], el_type);
    if (jl_is_bits_type(el_type)) {
        size_t nb = ((jl_bits_type_t*)el_type)->nbits/8;
        switch (nb) {
        case 1:
            ((int8_t*)b->data)[i]  = *(int8_t*)jl_bits_data(rhs);  break;
        case 2:
            ((int16_t*)b->data)[i] = *(int16_t*)jl_bits_data(rhs); break;
        case 4:
            ((int32_t*)b->data)[i] = *(int32_t*)jl_bits_data(rhs); break;
        case 8:
            ((int64_t*)b->data)[i] = *(int64_t*)jl_bits_data(rhs); break;
        default:
            memcpy(&((char*)b->data)[i*nb], jl_bits_data(rhs), nb);
        }
    }
    else {
        ((jl_value_t**)b->data)[i] = rhs;
    }
    return args[0];
}

JL_CALLABLE(jl_f_box)
{
    JL_NARGS(box, 1, 2);
    return nargs == 1 ?
        jl_new_struct((jl_struct_type_t*)jl_box_any_type, NULL) :
        jl_new_struct((jl_struct_type_t*)jl_box_any_type, args[1]);
}

JL_CALLABLE(jl_f_unbox)
{
    JL_NARGS(unbox, 1, 1);
    JL_TYPECHK(unbox, box, args[0]);
    jl_value_t *v = ((jl_value_t**)args[0])[1];
    if (v == NULL)
        jl_error("variable not defined");
    return v;
}

JL_CALLABLE(jl_f_boxset)
{
    JL_NARGS(boxset, 2, 2);
    JL_TYPECHK(boxset, box, args[0]);
    ((jl_value_t**)args[0])[1] = args[1];
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_instantiate_type)
{
    JL_NARGSV(instantiate_type, 1);
    JL_TYPECHK(instantiate_type, typector, args[0]);
    jl_tuple_t *tparams = (jl_tuple_t*)jl_f_tuple(NULL, &args[1], nargs-1);
    return (jl_value_t*)jl_apply_type_ctor((jl_typector_t*)args[0], tparams);
}

// --- conversions ---

static jl_tuple_t *convert_tuple(jl_tuple_t *x, jl_tuple_t *to)
{
    if (to == jl_tuple_type)
        return x;
    size_t i, cl=x->length, pl=to->length;
    jl_tuple_t *out = jl_alloc_tuple(cl);
    jl_value_t *ce, *pe;
    int pseq=0;
    for(i=0; i < cl; i++) {
        ce = jl_tupleref(x,i);
        if (pseq) {
        }
        else if (i < pl) {
            pe = jl_tupleref(to,i);
            if (jl_is_seq_type(pe)) {
                pe = jl_tparam0(pe);
                pseq = 1;
            }
        }
        else {
            return NULL;
        }
        jl_tupleset(out, i, jl_convert(ce, (jl_type_t*)pe));
    }
    return out;
}

jl_value_t *jl_convert(jl_value_t *x, jl_type_t *to)
{
    jl_value_t *out;
    if (jl_is_tuple(x) && jl_is_tuple(to)) {
        out = (jl_value_t*)convert_tuple((jl_tuple_t*)x, (jl_tuple_t*)to);
        if (out == NULL)
            jl_error("convert: invalid tuple conversion");
        return out;
    }
    jl_type_t *t = (jl_type_t*)jl_typeof(x);
    if (jl_subtype((jl_value_t*)t, (jl_value_t*)to, 0, 0))
        return x;
    jl_function_t *meth = NULL;
    if (jl_is_bits_type(to)) {
        meth = ((jl_bits_type_t*)to)->fconvert;
    }
    else if (jl_is_struct_type(to)) {
        meth = ((jl_struct_type_t*)to)->fconvert;
    }
    else {
        jl_error("convert: invalid conversion");
    }
    assert(meth != NULL);
    out = jl_apply(meth, &x, 1);
    if (!jl_subtype((jl_value_t*)jl_typeof(out), (jl_value_t*)to, 0, 0))
        jl_errorf("convert: conversion to %s failed",
                  jl_tname((jl_value_t*)to)->name->name);
    return out;
}

JL_CALLABLE(jl_f_convert)
{
    JL_NARGS(convert, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(convert, type, args[1]);
    return jl_convert(args[0], (jl_type_t*)args[1]);
}

/*
  we allow promotion to be covariant by default, i.e.
  T{S} is promotable to T{R} if S is promotable to R.
  for example the following diagram must commute:

        Int32 ---------------> Float64
          |                       |
          |                       |
          V                       V
    Complex{Int32} ------> Complex{Float64}

  The top arrow is explicitly declared.
  The left arrow is given by "Complex.convert(x::T) = Complex(x, T.convert(0))".
  The bottom arrow is provided by covariant promotion.
*/
static jl_value_t *bigger_type(jl_tag_type_t *t1, jl_tag_type_t *t2);
static int is_type_bigger(jl_value_t *a, jl_value_t *b)
{
    assert(jl_is_some_tag_type(a));
    assert(jl_is_some_tag_type(b));
    return (a == bigger_type((jl_tag_type_t*)a, (jl_tag_type_t*)b));
}
static jl_value_t *bigger_type(jl_tag_type_t *t1, jl_tag_type_t *t2)
{
    if (t1 == t2) return (jl_value_t*)t1;
    if (jl_is_bits_type(t1) && jl_is_bits_type(t2)) {
        if (((jl_bits_type_t*)t1)->nbits > ((jl_bits_type_t*)t2)->nbits)
            return (jl_value_t*)t1;
        if (((jl_bits_type_t*)t2)->nbits > ((jl_bits_type_t*)t1)->nbits)
            return (jl_value_t*)t2;
        return NULL;
    }
    jl_tag_type_t *t1_ = t1;
    jl_tag_type_t *t2_ = t2;
    while (1) {
        if (t1 == jl_any_type) {
            if (t2 != jl_any_type)
                return (jl_value_t*)t1_;
            break;
        }
        else if (t2 == jl_any_type) {
            return (jl_value_t*)t2_;
        }
        
        if (t1->name == t2->name) {
            size_t i;
            jl_tag_type_t *winner = NULL;
            for(i=0; i < t1->parameters->length; i++) {
                jl_value_t *p1 = jl_tupleref(t1->parameters, i);
                jl_value_t *p2 = jl_tupleref(t2->parameters, i);
                if (jl_types_equal(p1,p2))
                    continue;
                if (jl_is_some_tag_type(p1) && jl_is_some_tag_type(p2)) {
                    if (is_type_bigger(p1,p2)) {
                        if (winner == t2_)
                            return NULL;
                        winner = t1_;
                    }
                    else if (is_type_bigger(p2,p1)) {
                        if (winner == t1_)
                            return NULL;
                        winner = t2_;
                    }
                    else {
                        return NULL;
                    }
                }
                else {
                    return NULL;
                }
            }
            if (winner == NULL)  // types equal
                return (jl_value_t*)t1_;
            return (jl_value_t*)winner;
        }

        if (jl_type_morespecific((jl_value_t*)t1, (jl_value_t*)t2, 0 ,0))
            return (jl_value_t*)t2_;
        if (jl_type_morespecific((jl_value_t*)t2, (jl_value_t*)t1, 0, 0))
            return (jl_value_t*)t1_;
        t1 = t1->super;
        t2 = t2->super;
    }
    
    return NULL;
}

JL_CALLABLE(jl_f_promote)
{
    if (nargs == 0)
        return (jl_value_t*)jl_null;
    if (nargs == 1)
        return (jl_value_t*)jl_tuple(1, args[0]);
    size_t i;
    for(i=0; i < nargs; i++) {
        if (!jl_is_some_tag_type(jl_typeof(args[i])))
            jl_error("promotion not applicable to given types");
    }
    jl_value_t *t = bigger_type((jl_tag_type_t*)jl_typeof(args[0]),
                                (jl_tag_type_t*)jl_typeof(args[1]));
    if (t == NULL)
        jl_error("arguments have no common embedding type");
    for(i=2; i < nargs; i++) {
        t = bigger_type((jl_tag_type_t*)t,
                        (jl_tag_type_t*)jl_typeof(args[i]));
        if (t == NULL)
            jl_error("arguments have no common embedding type");
    }
    jl_tuple_t *result = jl_alloc_tuple(nargs);
    for(i=0; i < nargs; i++)
        jl_tupleset(result, i, jl_convert(args[i], (jl_type_t*)t));
    return (jl_value_t*)result;
}

// --- printing ---

jl_function_t *jl_print_gf;

ios_t *current_output_stream;  // TODO: thread-local

static void call_print(jl_value_t *v)
{
    jl_apply(jl_print_gf, &v, 1);
}

char *jl_print_to_string(jl_value_t *v)
{
    ios_t *prevs = current_output_stream;
    ios_t dest;
    ios_mem(&dest, 0);
    current_output_stream = &dest;
    // this is a long-winded unwind-protect
    // the reason for all this fuss is to reset the current output stream
    // if an error occurs during printing.
    jmp_buf *prevh = CurrentExceptionHandler;
    jmp_buf handler;
    CurrentExceptionHandler = &handler;
    if (!setjmp(handler)) {
        jl_print(v);
    }
    else {
        CurrentExceptionHandler = prevh;
        current_output_stream = prevs;
        longjmp(*CurrentExceptionHandler, 1);
    }
    CurrentExceptionHandler = prevh;
    current_output_stream = prevs;
    size_t n;
    return ios_takebuf(&dest, &n);
}

void jl_print(jl_value_t *v)
{
    call_print(v);
}

// comma_one prints a comma for 1 element, e.g. "(x,)"
static void print_tuple(jl_tuple_t *t, char opn, char cls, int comma_one)
{
    ios_t *s = current_output_stream;
    ios_putc(opn, s);
    size_t i, n=t->length;
    for(i=0; i < n; i++) {
        call_print(jl_tupleref(t, i));
        if ((i < n-1) || (n==1 && comma_one))
            ios_putc(',', s);
    }
    ios_putc(cls, s);
}

static void print_type(jl_value_t *t)
{
    ios_t *s = current_output_stream;
    if (jl_is_func_type(t)) {
        /*
        call_print((jl_value_t*)((jl_func_type_t*)t)->from);
        ios_write(s, "-->", 3);
        call_print((jl_value_t*)((jl_func_type_t*)t)->to);
        */
        ios_write(s, "Function", 8);
    }
    else if (jl_is_union_type(t)) {
        ios_write(s, "Union", 5);
        print_tuple(((jl_uniontype_t*)t)->types, '(', ')', 0);
    }
    else if (jl_is_seq_type(t)) {
        call_print(jl_tparam0(t));
        ios_write(s, "...", 3);
    }
    else {
        assert(jl_is_some_tag_type(t));
        ios_puts(((jl_tag_type_t*)t)->name->name->name, s);
        jl_tuple_t *p = jl_tparams(t);
        if (p->length > 0)
            print_tuple(p, '{', '}', 0);
    }
}

static void print_function(jl_value_t *v)
{
    ios_t *s = current_output_stream;
    if (jl_is_typector(v)) {
        jl_print((jl_value_t*)((jl_typector_t*)v)->body);
    }
    else if (jl_is_gf(v)) {
        ios_puts("#<generic-function ", s);
        ios_puts(jl_gf_name(v)->name, s);
        ios_putc('>', s);
#ifdef DEBUG
        ios_putc('\n', s);
        jl_print_method_table((jl_function_t*)v);
#endif
    }
    else {
        ios_puts("#<closure>", s);
    }
}

static void print_int(void *data, int nbits)
{
    ios_t *s = current_output_stream;
    switch (nbits) {
    case 8:
        ios_printf(s, "%hhd", *(int8_t*)data);
        break;
    case 16:
        ios_printf(s, "%hd", *(int16_t*)data);
        break;
    case 32:
        ios_printf(s, "%d", *(int32_t*)data);
        break;
    case 64:
        ios_printf(s, "%lld", *(int64_t*)data);
        break;
    default:
        jl_error("print: unsupported integer size");
    }
}

static void print_uint(void *data, int nbits)
{
    ios_t *s = current_output_stream;
    switch (nbits) {
    case 8:
        ios_printf(s, "%hhu", *(int8_t*)data);
        break;
    case 16:
        ios_printf(s, "%hu", *(int16_t*)data);
        break;
    case 32:
        ios_printf(s, "%u", *(int32_t*)data);
        break;
    case 64:
        ios_printf(s, "%llu", *(int64_t*)data);
        break;
    default:
        jl_error("print: unsupported integer size");
    }
}

static void print_float64(double d, int single)
{
    ios_t *s = current_output_stream;
    char buf[64];
    int ndec = single ? 8 : 16;
    if (!DFINITE(d)) {
        char *rep;
        if (isnan(d))
            rep = sign_bit(d) ? "-NaN" : "+NaN";
        else
            rep = sign_bit(d) ? "-Inf" : "+Inf";
        if (single)
            ios_printf(s, "float32(%s)", rep);
        else
            ios_puts(rep, s);
    }
    else if (d == 0) {
        if (1/d < 0)
            ios_puts("-0.0", s);
        else
            ios_puts("0.0", s);
    }
    else {
        snprint_real(buf, sizeof(buf), d, 0, ndec, 3, 10);
        int hasdec = (strpbrk(buf, ".eE") != NULL);
        ios_puts(buf, s);
        if (!hasdec) ios_puts(".0", s);
    }
}

JL_CALLABLE(jl_f_print_bool)
{
    ios_t *s = current_output_stream;
    if (jl_unbox_bool(args[0]) == 0)
        ios_puts("false", s);
    else
        ios_puts("true", s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_float32)
{
    print_float64((double)*(float*)jl_bits_data(args[0]), 1);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_float64)
{
    print_float64(*(double*)jl_bits_data(args[0]), 0);
    return (jl_value_t*)jl_null;
}

#define INT_PRINT_FUNC(sgn,nb)                  \
JL_CALLABLE(jl_f_print_##sgn##nb)               \
{                                               \
    print_##sgn(jl_bits_data(args[0]), nb);     \
    return (jl_value_t*)jl_null;                \
}

INT_PRINT_FUNC(int,8)
INT_PRINT_FUNC(uint,8)
INT_PRINT_FUNC(int,16)
INT_PRINT_FUNC(uint,16)
INT_PRINT_FUNC(int,32)
INT_PRINT_FUNC(uint,32)
INT_PRINT_FUNC(int,64)
INT_PRINT_FUNC(uint,64)

JL_CALLABLE(jl_f_print_symbol)
{
    ios_t *s = current_output_stream;
    ios_putc('`', s);
    ios_puts(((jl_sym_t*)args[0])->name, s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_typename)
{
    jl_print((jl_value_t*)((jl_typename_t*)args[0])->name);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_typevar)
{
    ios_t *s = current_output_stream;
    ios_puts(((jl_tvar_t*)args[0])->name->name, s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_linfo)
{
    ios_t *s = current_output_stream;
    ios_puts("AST(", s);
    jl_print(((jl_lambda_info_t*)args[0])->ast);
    ios_putc(')', s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_string)
{
    ios_t *s = current_output_stream;
    jl_array_t *b = (jl_array_t*)args[0];

    ios_write(s, (char*)b->data, b->length);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_print_any)
{
    JL_NARGS(print, 1, 1);
    // fallback for printing some other builtin types
    ios_t *s = current_output_stream;
    jl_value_t *v = args[0];
    if (jl_is_tuple(v)) {
        print_tuple((jl_tuple_t*)v, '(', ')', 1);
    }
    else if (jl_is_type(v)) {
        print_type(v);
    }
    else if (jl_is_func(v)) {
        print_function(v);
    }
    else if (jl_typeis(v,jl_intrinsic_type)) {
        ios_printf(s, "#<intrinsic-function %d>", *(uint32_t*)jl_bits_data(v));
    }
    else {
        jl_value_t *t = (jl_value_t*)jl_typeof(v);
        if (jl_is_bits_type(t)) {
            print_uint(jl_bits_data(v), ((jl_bits_type_t*)t)->nbits);
        }
        else {
            assert(jl_is_struct_type(t));
            jl_struct_type_t *st = (jl_struct_type_t*)t;
            ios_puts(st->name->name->name, s);
            ios_putc('(', s);
            size_t i;
            size_t n = st->names->length;
            for(i=0; i < n; i++) {
                call_print(((jl_value_t**)v)[i+1]);
                if (i < n-1)
                    ios_write(s, ",", 2);
            }
            ios_putc(')', s);
        }
    }
    return (jl_value_t*)jl_null;
}

// --- RTS primitives ---

JL_CALLABLE(jl_trampoline)
{
    jl_function_t *f = (jl_function_t*)jl_t0(env);
    assert(jl_is_func(f));
    assert(f->linfo != NULL);
    jl_value_t *cloenv = jl_t1(env);
    jl_compile(f->linfo);
    assert(f->linfo->fptr != NULL);
    f->fptr = f->linfo->fptr;
    f->env = cloenv;
    return jl_apply(f, args, nargs);
}

JL_CALLABLE(jl_f_new_closure)
{
    JL_NARGS(new_closure, 2, 2);
    JL_TYPECHK(new_closure, tuple, args[1]);
    assert(jl_is_lambda_info(args[0]));
    jl_lambda_info_t *li = (jl_lambda_info_t*)args[0];
    jl_function_t *f = jl_new_closure(NULL, NULL);
    f->linfo = li;
    if (li->fptr != NULL) {
        // function has been compiled
        f->fptr = li->fptr;
        f->env = args[1];
    }
    else {
        f->fptr = jl_trampoline;
        f->env = (jl_value_t*)jl_pair((jl_value_t*)f, args[1]);
    }
    return (jl_value_t*)f;
}

static int all_typevars(jl_tuple_t *p)
{
    size_t i;
    for(i=0; i < p->length; i++) {
        if (!jl_is_typevar(jl_tupleref(p,i)))
            return 0;
    }
    return 1;
}

static void check_supertype(jl_value_t *super, char *name)
{
    if (!(/*jl_is_struct_type(super) || */jl_is_tag_type(super)) ||
        super == (jl_value_t*)jl_sym_type ||
        jl_subtype(super,(jl_value_t*)jl_type_type,0,0) ||
        jl_subtype(super,(jl_value_t*)jl_array_type,0,0)) {
        jl_errorf("invalid subtyping in definition of %s", name);
    }
}

JL_CALLABLE(jl_f_new_struct_type)
{
    JL_NARGS(new_struct_type, 4, 4);
    JL_TYPECHK(new_struct_type, symbol, args[0]);
    JL_TYPECHK(new_struct_type, tuple, args[2]);
    JL_TYPECHK(new_struct_type, tuple, args[3]);
    jl_sym_t *name = (jl_sym_t*)args[0];
    jl_tuple_t *params = (jl_tuple_t*)args[2];
    jl_tuple_t *fnames = (jl_tuple_t*)args[3];
    if (!all_typevars(params))
        jl_errorf("invalid type parameter list for %s", name->name);
    jl_value_t *super = args[1];

    jl_struct_type_t *nst =
        jl_new_struct_type(name, jl_any_type, params, jl_null, NULL);
    if (super == (jl_value_t*)jl_scalar_type ||
        super == (jl_value_t*)jl_number_type ||
        super == (jl_value_t*)jl_real_type ||
        super == (jl_value_t*)jl_int_type ||
        super == (jl_value_t*)jl_float_type) {
        nst->super = (jl_tag_type_t*)jl_apply_type_ctor((jl_typector_t*)super,
                                                        jl_tuple(1, nst));
        nst->names = fnames;
    }
    else {
        assert(jl_is_type(args[1]));
        check_supertype(super, name->name);
        nst->super = (jl_tag_type_t*)super;
        if (jl_is_struct_type(super)) {
            nst->names = jl_tuple_append(((jl_struct_type_t*)super)->names,
                                         fnames);
        }
        else {
            assert(jl_is_tag_type(super));
            nst->names = fnames;
        }
    }
    return (jl_value_t*)nst;
}

void jl_add_generic_constructor(jl_typector_t *tc);

JL_CALLABLE(jl_f_new_struct_fields)
{
    JL_NARGS(new_struct_fields, 2, 2);
    JL_TYPECHK(new_struct_fields, tuple, args[1]);
    jl_value_t *tc = args[0];
    jl_value_t *t = args[0];
    jl_tuple_t *ftypes = (jl_tuple_t*)args[1];
    assert(jl_is_typector(t));
    t = (jl_value_t*)((jl_typector_t*)t)->body;
    if (!jl_is_struct_type(t))
        jl_error("you can't do that.");
    jl_struct_type_t *st = (jl_struct_type_t*)t;
    if (st->types != NULL)
        jl_error("you can't do that.");
    jl_tuple_t *pft = NULL;
    jl_tag_type_t *super = st->super;
    if (jl_is_struct_type(super))
        pft = ((jl_struct_type_t*)super)->types;
    else if (jl_is_tag_type(super))
        pft = jl_null;
    else
        assert(0);
    st->types = jl_tuple_append(pft, ftypes);
    jl_add_generic_constructor((jl_typector_t*)tc);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_new_type_constructor)
{
    JL_NARGS(new_type_constructor, 2, 2);
    JL_TYPECHK(new_type_constructor, tuple, args[0]);
    assert(jl_is_type(args[1]));
    jl_tuple_t *p = (jl_tuple_t*)args[0];
    if (!all_typevars(p)) {
        jl_errorf("invalid type parameter list for %s",
                  jl_tname(args[1])->name->name);
    }
    return (jl_value_t*)jl_new_type_ctor(p, (jl_type_t*)args[1]);
}

JL_CALLABLE(jl_f_new_tag_type)
{
    JL_NARGS(new_tag_type, 3, 3);
    JL_TYPECHK(new_tag_type, symbol, args[0]);
    JL_TYPECHK(new_tag_type, tag_type, args[1]);
    JL_TYPECHK(new_tag_type, tuple, args[2]);
    jl_tuple_t *p = (jl_tuple_t*)args[2];
    if (!all_typevars(p)) {
        jl_errorf("invalid type parameter list for %s",
                  ((jl_sym_t*)args[0])->name);
    }
    jl_value_t *super = args[1];
    check_supertype(super, ((jl_sym_t*)args[0])->name);
    return (jl_value_t*)jl_new_tagtype((jl_value_t*)args[0], (jl_tag_type_t*)super, p);
}

JL_CALLABLE(jl_f_typevar)
{
    JL_NARGS(typevar, 1, 1);
    JL_TYPECHK(typevar, symbol, args[0]);
    return (jl_value_t*)jl_new_struct(jl_tvar_type, (jl_sym_t*)args[0],
                                      jl_bottom_type, jl_any_type);
}

JL_CALLABLE(jl_f_union)
{
    if (nargs == 1) return args[0];
    size_t i;
    jl_tuple_t *argt = jl_alloc_tuple(nargs);
    for(i=0; i < nargs; i++) {
        if (jl_is_typector(args[i])) {
            jl_tupleset(argt,i,jl_unconstrained_type((jl_typector_t*)args[i]));
        }
        else if (!jl_is_type(args[i]) && !jl_is_typevar(args[i])) {
            jl_error("invalid union type");
        }
        else {
            jl_tupleset(argt, i, args[i]);
        }
    }
    argt = jl_compute_type_union(argt);
    if (argt->length == 1)
        return jl_tupleref(argt, 0);
    return (jl_value_t*)jl_new_uniontype(argt);
}

JL_CALLABLE(jl_f_new_generic_function)
{
    JL_NARGS(new_generic_function, 1, 1);
    JL_TYPECHK(new_generic_function, symbol, args[0]);
    return (jl_value_t*)jl_new_generic_function((jl_sym_t*)args[0]);
}

JL_CALLABLE(jl_f_add_method)
{
    JL_NARGS(add_method, 3, 3);
    if (!jl_is_gf(args[0]))
        jl_error("add_method: not a generic function");
    JL_TYPECHK(add_method, tuple, args[1]);
    JL_TYPECHK(add_method, function, args[2]);
    jl_add_method((jl_function_t*)args[0], (jl_tuple_t*)args[1],
                  (jl_function_t*)args[2]);
    return args[0];
}

// --- init ---

static void add_builtin_method1(jl_function_t *gf, jl_type_t *t, jl_fptr_t f)
{
    jl_add_method(gf, jl_tuple(1, t), jl_new_closure(f, NULL));
}

static void add_builtin(const char *name, jl_value_t *v)
{
    jl_set_const(jl_system_module, jl_symbol(name), v);
}

static void add_builtin_func(const char *name, jl_fptr_t f)
{
    add_builtin(name, (jl_value_t*)jl_new_closure(f, NULL));
}

void jl_init_builtins()
{
    jl_print_gf = jl_new_generic_function(jl_symbol("print"));

    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_any_type,     jl_f_print_any);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_sym_type,     jl_f_print_symbol);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_typename_type,jl_f_print_typename);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_tvar_type,    jl_f_print_typevar);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_lambda_info_type, jl_f_print_linfo);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_array_uint8_type,  jl_f_print_string);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_float32_type, jl_f_print_float32);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_float64_type, jl_f_print_float64);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_int8_type,    jl_f_print_int8);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_uint8_type,   jl_f_print_uint8);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_int16_type,   jl_f_print_int16);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_uint16_type,  jl_f_print_uint16);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_int32_type,   jl_f_print_int32);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_uint32_type,  jl_f_print_uint32);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_int64_type,   jl_f_print_int64);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_uint64_type,  jl_f_print_uint64);
    add_builtin_method1(jl_print_gf, (jl_type_t*)jl_bool_type,    jl_f_print_bool);

    current_output_stream = ios_stdout;

    add_builtin_func("is", jl_f_is);
    add_builtin_func("typeof", jl_f_typeof);
    add_builtin_func("subtype", jl_f_subtype);
    add_builtin_func("istype", jl_f_istype);
    add_builtin_func("typeassert", jl_f_typeassert);
    add_builtin_func("apply", jl_f_apply);
    add_builtin_func("error", jl_f_error);
    add_builtin_func("load", jl_f_load);
    add_builtin_func("tuple", jl_f_tuple);
    add_builtin_func("convert", jl_f_convert);
    add_builtin_func("promote", jl_f_promote);
    add_builtin_func("Union", jl_f_union);
    add_builtin_func("time_thunk", jl_f_time_thunk);
    add_builtin("print", (jl_value_t*)jl_print_gf);
    add_builtin("identity", (jl_value_t*)jl_identity_func);
    
    // functions for internal use
    add_builtin_func("tupleref", jl_f_tupleref);
    add_builtin_func("tuplelen", jl_f_tuplelen);
    add_builtin_func("getfield", jl_f_get_field);
    add_builtin_func("setfield", jl_f_set_field);
    add_builtin_func("arraylen", jl_f_arraylen);
    add_builtin_func("arrayref", jl_f_arrayref);
    add_builtin_func("arrayset", jl_f_arrayset);
    add_builtin_func("box", jl_f_box);
    add_builtin_func("unbox", jl_f_unbox);
    add_builtin_func("boxset", jl_f_boxset);
    add_builtin_func("instantiate_type", jl_f_instantiate_type);
    add_builtin_func("typevar", jl_f_typevar);
    add_builtin_func("new_closure", jl_f_new_closure);
    add_builtin_func("new_struct_type", jl_f_new_struct_type);
    add_builtin_func("new_struct_fields", jl_f_new_struct_fields);
    add_builtin_func("new_type_constructor", jl_f_new_type_constructor);
    add_builtin_func("new_tag_type", jl_f_new_tag_type);
    add_builtin_func("new_generic_function", jl_f_new_generic_function);
    add_builtin_func("add_method", jl_f_add_method);

    // builtin types
    add_builtin("Any", (jl_value_t*)jl_any_type);
    add_builtin("Bottom", (jl_value_t*)jl_bottom_type);
    add_builtin("TypeVar", (jl_value_t*)jl_tvar_type);
    add_builtin("Tuple", (jl_value_t*)jl_tuple_type);
    add_builtin("NTuple", (jl_value_t*)jl_ntuple_type);
    add_builtin("Type", (jl_value_t*)jl_type_type);
    add_builtin("Symbol", (jl_value_t*)jl_sym_type);
    add_builtin("...", (jl_value_t*)jl_seq_type);
    add_builtin("Function", (jl_value_t*)jl_any_func);
    add_builtin("Array", (jl_value_t*)jl_array_type);
    add_builtin("Tensor", (jl_value_t*)jl_tensor_type);
    add_builtin("Scalar", (jl_value_t*)jl_scalar_type);
    add_builtin("Number", (jl_value_t*)jl_number_type);
    add_builtin("Real", (jl_value_t*)jl_real_type);
    add_builtin("Int", (jl_value_t*)jl_int_type);
    add_builtin("Float", (jl_value_t*)jl_float_type);
    add_builtin("Bool", (jl_value_t*)jl_bool_type);
    add_builtin("Int8", (jl_value_t*)jl_int8_type);
    add_builtin("Uint8", (jl_value_t*)jl_uint8_type);
    add_builtin("Int16", (jl_value_t*)jl_int16_type);
    add_builtin("Uint16", (jl_value_t*)jl_uint16_type);
    add_builtin("Int32", (jl_value_t*)jl_int32_type);
    add_builtin("Uint32", (jl_value_t*)jl_uint32_type);
    add_builtin("Int64", (jl_value_t*)jl_int64_type);
    add_builtin("Uint64", (jl_value_t*)jl_uint64_type);
    add_builtin("Float32", (jl_value_t*)jl_float32_type);
    add_builtin("Float64", (jl_value_t*)jl_float64_type);

    add_builtin("Expr", (jl_value_t*)jl_new_type_ctor(jl_null,
                                                      (jl_type_t*)jl_expr_type));

    add_builtin("BitsKind", (jl_value_t*)jl_bits_kind);
    add_builtin("StructKind", (jl_value_t*)jl_struct_kind);
    add_builtin("FuncKind", (jl_value_t*)jl_func_kind);
    add_builtin("TagKind", (jl_value_t*)jl_tag_kind);
    add_builtin("UnionKind", (jl_value_t*)jl_union_kind);
}
