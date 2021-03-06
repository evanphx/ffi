/*
 * Copyright (c) 2008, 2009, Wayne Meissner
 * Copyright (c) 2009, Luc Heinrich <luc@honk-honk.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * The name of the author or authors may not be used to endorse or promote
 *   products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <stdint.h>
#include <stdbool.h>
#include <ruby.h>
#include "rbffi.h"
#include "compat.h"
#include "AbstractMemory.h"
#include "Pointer.h"
#include "MemoryPointer.h"
#include "Types.h"
#include "Struct.h"

typedef struct StructLayoutBuilder {
    unsigned int offset;
} StructLayoutBuilder;

static void struct_mark(Struct *);
static void struct_layout_mark(StructLayout *);
static void struct_layout_free(StructLayout *);
static void struct_field_mark(StructField* f);
static inline MemoryOp* ptr_get_op(AbstractMemory* ptr, Type* type);

VALUE rbffi_StructClass = Qnil;
VALUE rbffi_StructLayoutClass = Qnil;
static VALUE StructFieldClass = Qnil, StructLayoutBuilderClass = Qnil;
static ID id_pointer_ivar = 0, id_layout_ivar = 0, TYPE_ID;
static ID id_get = 0, id_put = 0, id_to_ptr = 0, id_to_s = 0, id_layout = 0;

static VALUE
struct_field_allocate(VALUE klass)
{
    StructField* field;
    VALUE obj;
    
    obj = Data_Make_Struct(klass, StructField, struct_field_mark, -1, field);
    field->rbType = Qnil;

    return obj;
}

static void
struct_field_mark(StructField* f)
{
    rb_gc_mark(f->rbType);
}

static VALUE
struct_field_initialize(int argc, VALUE* argv, VALUE self)
{
    VALUE offset = Qnil, info = Qnil;
    StructField* field;
    int nargs;

    Data_Get_Struct(self, StructField, field);

    nargs = rb_scan_args(argc, argv, "11", &offset, &info);
    
    field->offset = NUM2UINT(offset);
    if (rb_const_defined(CLASS_OF(self), TYPE_ID)) {
        field->rbType = rbffi_Type_Find(rb_const_get(CLASS_OF(self), TYPE_ID));
        Data_Get_Struct(field->rbType, Type, field->type);
    } else {
        field->rbType = Qnil;
        field->type = NULL;
    }

    rb_iv_set(self, "@off", offset);
    rb_iv_set(self, "@info", info);

    return self;
}

static VALUE
struct_field_offset(VALUE self)
{
    StructField* field;
    Data_Get_Struct(self, StructField, field);
    return UINT2NUM(field->offset);
}

static VALUE
struct_field_get(VALUE self, VALUE pointer)
{
    StructField* f;
    MemoryOp* op;
    AbstractMemory* memory = MEMORY(pointer);

    Data_Get_Struct(self, StructField, f);
    op = ptr_get_op(memory, f->type);
    if (op == NULL) {
        VALUE name = rb_class_name(CLASS_OF(self));
        rb_raise(rb_eArgError, "get not supported for %s", StringValueCStr(name));
        return Qnil;
    }

    return (*op->get)(memory, f->offset);
}

static VALUE
struct_field_put(VALUE self, VALUE pointer, VALUE value)
{
    StructField* f;
    MemoryOp* op;
    AbstractMemory* memory = MEMORY(pointer);

    Data_Get_Struct(self, StructField, f);
    op = ptr_get_op(memory, f->type);
    if (op == NULL) {
        VALUE name = rb_class_name(CLASS_OF(self));
        rb_raise(rb_eArgError, "put not supported for %s", StringValueCStr(name));
        return self;
    }
    
    (*op->put)(memory, f->offset, value);

    return self;
}

static inline char*
memory_address(VALUE self)
{
    return ((AbstractMemory *)DATA_PTR((self)))->address;
}

static VALUE
struct_allocate(VALUE klass)
{
    Struct* s;
    VALUE obj = Data_Make_Struct(klass, Struct, struct_mark, -1, s);
    
    s->rbPointer = Qnil;
    s->rbLayout = Qnil;

    return obj;
}

static VALUE
struct_initialize(int argc, VALUE* argv, VALUE self)
{
    Struct* s;
    VALUE rbPointer = Qnil, rest = Qnil, klass = CLASS_OF(self);
    int nargs;

    Data_Get_Struct(self, Struct, s);
    
    nargs = rb_scan_args(argc, argv, "01*", &rbPointer, &rest);

    /* Call up into ruby code to adjust the layout */
    if (nargs > 1) {
        s->rbLayout = rb_funcall2(CLASS_OF(self), id_layout, RARRAY_LEN(rest), RARRAY_PTR(rest));
    } else if (rb_cvar_defined(klass, id_layout_ivar)) {
        s->rbLayout = rb_cvar_get(klass, id_layout_ivar);
    } else {
        rb_raise(rb_eRuntimeError, "No Struct layout configured");
    }

    if (!rb_obj_is_kind_of(s->rbLayout, rbffi_StructLayoutClass)) {
        rb_raise(rb_eRuntimeError, "Invalid Struct layout");
    }

    Data_Get_Struct(s->rbLayout, StructLayout, s->layout);
    
    if (rbPointer != Qnil) {
        s->pointer = MEMORY(rbPointer);
        s->rbPointer = rbPointer;
    } else {
        s->rbPointer = rbffi_MemoryPointer_NewInstance(s->layout->size, 1, true);
        s->pointer = (AbstractMemory *) DATA_PTR(s->rbPointer);
    }

    if (s->pointer->ops == NULL) {
        VALUE name = rb_class_name(CLASS_OF(s->rbPointer));
        rb_raise(rb_eRuntimeError, "No memory ops set for %s", StringValueCStr(name));
    }

    return self;
}

static void
struct_mark(Struct *s)
{
    rb_gc_mark(s->rbPointer);
    rb_gc_mark(s->rbLayout);
}

static VALUE
struct_field(Struct* s, VALUE fieldName)
{
    StructLayout* layout = s->layout;
    VALUE rbField;
    if (layout == NULL) {
        rb_raise(rb_eRuntimeError, "layout not set for Struct");
    }

    rbField = rb_hash_aref(layout->rbFieldMap, fieldName);
    if (rbField == Qnil) {
        VALUE str = rb_funcall2(fieldName, id_to_s, 0, NULL);
        rb_raise(rb_eArgError, "No such field '%s'", StringValuePtr(str));
    }

    return rbField;
}

static inline MemoryOp*
ptr_get_op(AbstractMemory* ptr, Type* type)
{
    if (ptr == NULL || ptr->ops == NULL || type == NULL) {
        return NULL;
    }
    switch (type->nativeType) {
        case NATIVE_INT8:
            return ptr->ops->int8;
        case NATIVE_UINT8:
            return ptr->ops->uint8;
        case NATIVE_INT16:
            return ptr->ops->int16;
        case NATIVE_UINT16:
            return ptr->ops->uint16;
        case NATIVE_INT32:
            return ptr->ops->int32;
        case NATIVE_UINT32:
            return ptr->ops->uint32;
        case NATIVE_INT64:
            return ptr->ops->int64;
        case NATIVE_UINT64:
            return ptr->ops->uint64;
        case NATIVE_FLOAT32:
            return ptr->ops->float32;
        case NATIVE_FLOAT64:
            return ptr->ops->float64;
        case NATIVE_POINTER:
            return ptr->ops->pointer;
        case NATIVE_STRING:
            return ptr->ops->strptr;
        default:
            return NULL;
    }
}

static VALUE
struct_get_field(VALUE self, VALUE fieldName)
{
    Struct* s;
    VALUE rbField;
    StructField* f;
    MemoryOp* op;

    Data_Get_Struct(self, Struct, s);
    rbField = struct_field(s, fieldName);
    f = (StructField *) DATA_PTR(rbField);

    op = ptr_get_op(s->pointer, f->type);
    if (op != NULL) {
        return (*op->get)(s->pointer, f->offset);
    }
    
    /* call up to the ruby code to fetch the value */
    return rb_funcall2(rbField, id_get, 1, &s->rbPointer);
}

static VALUE
struct_put_field(VALUE self, VALUE fieldName, VALUE value)
{
    Struct* s;
    VALUE rbField;
    StructField* f;
    MemoryOp* op;
    VALUE argv[2];

    Data_Get_Struct(self, Struct, s);
    rbField = struct_field(s, fieldName);
    f = (StructField *) DATA_PTR(rbField);

    op = ptr_get_op(s->pointer, f->type);
    if (op != NULL) {
        (*op->put)(s->pointer, f->offset, value);
        return self;
    }
    
    /* call up to the ruby code to set the value */
    argv[0] = s->rbPointer;
    argv[1] = value;
    rb_funcall2(rbField, id_put, 2, argv);
    
    return self;
}

static VALUE
struct_set_pointer(VALUE self, VALUE pointer)
{
    Struct* s;

    if (!rb_obj_is_kind_of(pointer, rbffi_AbstractMemoryClass)) {
        rb_raise(rb_eArgError, "Invalid pointer");
    }

    Data_Get_Struct(self, Struct, s);
    s->pointer = MEMORY(pointer);
    s->rbPointer = pointer;
    rb_ivar_set(self, id_pointer_ivar, pointer);

    return self;
}

static VALUE
struct_get_pointer(VALUE self)
{
    Struct* s;

    Data_Get_Struct(self, Struct, s);

    return s->rbPointer;
}

static VALUE
struct_set_layout(VALUE self, VALUE layout)
{
    Struct* s;
    Data_Get_Struct(self, Struct, s);

    if (!rb_obj_is_kind_of(layout, rbffi_StructLayoutClass)) {
        rb_raise(rb_eArgError, "Invalid Struct layout");
    }

    Data_Get_Struct(layout, StructLayout, s->layout);
    rb_ivar_set(self, id_layout_ivar, layout);

    return self;
}

static VALUE
struct_get_layout(VALUE self)
{
    Struct* s;

    Data_Get_Struct(self, Struct, s);

    return s->rbLayout;
}

static VALUE
struct_layout_allocate(VALUE klass)
{
    StructLayout* layout;
    VALUE obj;
    
    obj = Data_Make_Struct(klass, StructLayout, struct_layout_mark, struct_layout_free, layout);
    layout->rbFieldMap = Qnil;

    return obj;
}

static VALUE
struct_layout_initialize(VALUE self, VALUE field_names, VALUE fields, VALUE size, VALUE align)
{
    StructLayout* layout;
    int i;

    Data_Get_Struct(self, StructLayout, layout);
    layout->rbFieldMap = rb_hash_new();
    layout->size = NUM2INT(size);
    layout->align = NUM2INT(align);
    layout->fieldCount = RARRAY_LEN(field_names);
    layout->fields = ALLOC_N(StructField*, layout->fieldCount);
    if (layout->fields == NULL) {
        rb_raise(rb_eNoMemError, "failed to allocate memory for %d fields", layout->fieldCount);
    }

    rb_iv_set(self, "@field_names", field_names);
    rb_iv_set(self, "@fields", fields);
    rb_iv_set(self, "@size", size);
    rb_iv_set(self, "@align", align);

    for (i = 0; i < layout->fieldCount; ++i) {
        VALUE name = rb_ary_entry(field_names, i);
        VALUE field = rb_hash_aref(fields, name);
        if (TYPE(field) != T_DATA || !rb_obj_is_kind_of(field, StructFieldClass)) {
            rb_raise(rb_eArgError, "Invalid field");
        }
        rb_hash_aset(layout->rbFieldMap, name, field);
        Data_Get_Struct(field, StructField, layout->fields[i]);
    }

    return self;
}

static VALUE
struct_layout_aref(VALUE self, VALUE field)
{
    StructLayout* layout;

    Data_Get_Struct(self, StructLayout, layout);

    return rb_hash_aref(layout->rbFieldMap, field);
}


static void
struct_layout_mark(StructLayout *layout)
{
    rb_gc_mark(layout->rbFieldMap);
}

static void
struct_layout_free(StructLayout *layout)
{
    xfree(layout->fields);
    xfree(layout);
}

void
rbffi_Struct_Init(VALUE moduleFFI)
{
    VALUE klass, StructClass;
    rbffi_StructClass = StructClass = rb_define_class_under(moduleFFI, "Struct", rb_cObject);
    rb_global_variable(&rbffi_StructClass);

    rbffi_StructLayoutClass = rb_define_class_under(moduleFFI, "StructLayout", rb_cObject);
    rb_global_variable(&rbffi_StructLayoutClass);

    StructLayoutBuilderClass = rb_define_class_under(moduleFFI, "StructLayoutBuilder", rb_cObject);
    rb_global_variable(&StructLayoutBuilderClass);

    StructFieldClass = rb_define_class_under(StructLayoutBuilderClass, "Field", rb_cObject);
    rb_global_variable(&StructFieldClass);

    rb_define_alloc_func(StructClass, struct_allocate);
    rb_define_method(StructClass, "initialize", struct_initialize, -1);
    
    rb_define_alias(rb_singleton_class(StructClass), "alloc_in", "new");
    rb_define_alias(rb_singleton_class(StructClass), "alloc_out", "new");
    rb_define_alias(rb_singleton_class(StructClass), "alloc_inout", "new");
    rb_define_alias(rb_singleton_class(StructClass), "new_in", "new");
    rb_define_alias(rb_singleton_class(StructClass), "new_out", "new");
    rb_define_alias(rb_singleton_class(StructClass), "new_inout", "new");

    rb_define_method(StructClass, "pointer", struct_get_pointer, 0);
    rb_define_private_method(StructClass, "pointer=", struct_set_pointer, 1);

    rb_define_method(StructClass, "layout", struct_get_layout, 0);
    rb_define_private_method(StructClass, "layout=", struct_set_layout, 1);

    rb_define_method(StructClass, "[]", struct_get_field, 1);
    rb_define_method(StructClass, "[]=", struct_put_field, 2);
    
    rb_define_alloc_func(StructFieldClass, struct_field_allocate);
    rb_define_method(StructFieldClass, "initialize", struct_field_initialize, -1);
    rb_define_method(StructFieldClass, "offset", struct_field_offset, 0);
    rb_define_method(StructFieldClass, "put", struct_field_put, 2);
    rb_define_method(StructFieldClass, "get", struct_field_get, 1);

    rb_define_alloc_func(rbffi_StructLayoutClass, struct_layout_allocate);
    rb_define_method(rbffi_StructLayoutClass, "initialize", struct_layout_initialize, 4);
    rb_define_method(rbffi_StructLayoutClass, "[]", struct_layout_aref, 1);

    id_pointer_ivar = rb_intern("@pointer");
    id_layout_ivar = rb_intern("@layout");
    id_layout = rb_intern("layout");
    id_get = rb_intern("get");
    id_put = rb_intern("put");
    id_to_ptr = rb_intern("to_ptr");
    id_to_s = rb_intern("to_s");
    TYPE_ID = rb_intern("TYPE");
#undef FIELD
#define FIELD(name, typeName, nativeType, T) do { \
    typedef struct { char c; T v; } s; \
        klass = rb_define_class_under(StructLayoutBuilderClass, #name, StructFieldClass); \
        rb_define_const(klass, "TYPE", rb_const_get(moduleFFI, rb_intern("TYPE_"#nativeType))); \
    } while(0)
    
    FIELD(Signed8, int8, INT8, char);
    FIELD(Unsigned8, uint8, UINT8, unsigned char);
    FIELD(Signed16, int16, INT16, short);
    FIELD(Unsigned16, uint16, UINT16, unsigned short);
    FIELD(Signed32, int32, INT32, int);
    FIELD(Unsigned32, uint32, UINT32, unsigned int);
    FIELD(Signed64, int64, INT64, long long);
    FIELD(Unsigned64, uint64, UINT64, unsigned long long);
    FIELD(FloatField, float32, FLOAT32, float);
    FIELD(DoubleField, float64, FLOAT64, double);
    FIELD(PointerField, pointer, POINTER, char *);
    FIELD(StringField, string, STRING, char *);
}
