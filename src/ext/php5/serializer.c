#include <Zend/zend.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <php.h>

#include <ext/spl/spl_exceptions.h>

#include "arrays.h"
#include "compat_string.h"
#include "compatibility.h"
#include "ddtrace.h"
#include "logging.h"
#include "mpack/mpack.h"
#include "span.h"

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

static int msgpack_write_zval(mpack_writer_t *writer, zval *trace TSRMLS_DC);

static int write_hash_table(mpack_writer_t *writer, HashTable *ht TSRMLS_DC) {
    zval **tmp;
    char *string_key;
    uint str_len;
    HashPosition iterator;
    zend_ulong num_key;
    int key_type;
    bool first_time = true;

    zend_hash_internal_pointer_reset_ex(ht, &iterator);
    while (zend_hash_get_current_data_ex(ht, (void **)&tmp, &iterator) == SUCCESS) {
        key_type = zend_hash_get_current_key_ex(ht, &string_key, &str_len, &num_key, 0, &iterator);
        if (first_time == true) {
            first_time = false;
            if (key_type == HASH_KEY_IS_STRING) {
                mpack_start_map(writer, zend_hash_num_elements(ht));
            } else {
                mpack_start_array(writer, zend_hash_num_elements(ht));
            }
        }
        if (key_type == HASH_KEY_IS_STRING) {
            mpack_write_cstr(writer, string_key);
        }
        if (msgpack_write_zval(writer, *tmp TSRMLS_CC) != 1) {
            return 0;
        }
        zend_hash_move_forward_ex(ht, &iterator);
    }

    if (key_type == HASH_KEY_IS_STRING) {
        mpack_finish_map(writer);
    } else {
        mpack_finish_array(writer);
    }
    return 1;
}

static int msgpack_write_zval(mpack_writer_t *writer, zval *trace TSRMLS_DC) {
    switch (Z_TYPE_P(trace)) {
        case IS_ARRAY:
            if (write_hash_table(writer, Z_ARRVAL_P(trace) TSRMLS_CC) != 1) {
                return 0;
            }
            break;
        case IS_DOUBLE:
            mpack_write_double(writer, Z_DVAL_P(trace));
            break;
        case IS_LONG:
            mpack_write_int(writer, Z_LVAL_P(trace));
            break;
        case IS_NULL:
            mpack_write_nil(writer);
            break;
        case IS_BOOL:
            mpack_write_bool(writer, Z_BVAL_P(trace) == 1);
            break;
        case IS_STRING:
            mpack_write_cstr(writer, Z_STRVAL_P(trace));
            break;
        default:
            ddtrace_log_debug("Serialize values must be of type array, string, int, float, bool or null");
            return 0;
            break;
    }
    return 1;
}

int ddtrace_serialize_simple_array_into_c_string(zval *trace, char **data_p, size_t *size_p TSRMLS_DC) {
    // encode to memory buffer
    char *data;
    size_t size;
    mpack_writer_t writer;
    mpack_writer_init_growable(&writer, &data, &size);
    if (msgpack_write_zval(&writer, trace TSRMLS_CC) != 1) {
        mpack_writer_destroy(&writer);
        free(data);
        return 0;
    }
    // finish writing
    if (mpack_writer_destroy(&writer) != mpack_ok) {
        free(data);
        return 0;
    }

    if (data_p && size_p) {
        *data_p = data;
        *size_p = size;

        return 1;
    } else {
        return 0;
    }
}

int ddtrace_serialize_simple_array(zval *trace, zval *retval TSRMLS_DC) {
    // encode to memory buffer
    char *data;
    size_t size;

    if (ddtrace_serialize_simple_array_into_c_string(trace, &data, &size TSRMLS_CC)) {
        ZVAL_STRINGL(retval, data, size, 1);
        free(data);
        return 1;
    } else {
        return 0;
    }
}

static zval *_read_span_property(zval *span_data, const char *name, size_t name_len TSRMLS_DC) {
    return zend_read_property(ddtrace_ce_span_data, span_data, name, name_len, 1 TSRMLS_CC);
}

static void _add_assoc_zval_copy(zval *el, const char *name, zval *prop) {
    zval *value;
    ALLOC_ZVAL(value);
    INIT_PZVAL_COPY(value, prop);
    zval_copy_ctor(value);
    add_assoc_zval(el, name, value);
}

/* gettraceasstring() macros
 * @see https://github.com/php/php-src/blob/PHP-5.4/Zend/zend_exceptions.c#L364-L395
 * {{{ */
#define _DD_TRACE_APPEND_STRL(val, vallen)           \
    {                                                \
        int l = vallen;                              \
        *str = (char *)erealloc(*str, *len + l + 1); \
        memcpy((*str) + *len, val, l);               \
        *len += l;                                   \
    }

#define _DD_TRACE_APPEND_STR(val) _DD_TRACE_APPEND_STRL(val, sizeof(val) - 1)

#define _DD_TRACE_APPEND_KEY(key)                                          \
    if (zend_hash_find(ht, key, sizeof(key), (void **)&tmp) == SUCCESS) {  \
        if (Z_TYPE_PP(tmp) != IS_STRING) {                                 \
            /* zend_error(E_WARNING, "Value for %s is no string", key); */ \
            _DD_TRACE_APPEND_STR("[unknown]");                             \
        } else {                                                           \
            _DD_TRACE_APPEND_STRL(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));     \
        }                                                                  \
    }
/* }}} */

/* This is modelled after _build_trace_string in PHP 5.4
 * @see https://github.com/php/php-src/blob/PHP-5.4/Zend/zend_exceptions.c#L543-L605
 */
static int _trace_string(zval **frame TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key) {
    PHP5_UNUSED(hash_key);
#ifdef ZTS
    /* This arg is required for the zend_hash_apply_with_arguments function signature, but we don't need it */
    PHP5_UNUSED(TSRMLS_C);
#endif

    char *s_tmp, **str;
    int *len, *num;
    long line;
    HashTable *ht = Z_ARRVAL_PP(frame);
    zval **file, **tmp;

    if (Z_TYPE_PP(frame) != IS_ARRAY || num_args != 3) {
        return ZEND_HASH_APPLY_KEEP;
    }

    str = va_arg(args, char **);
    len = va_arg(args, int *);
    num = va_arg(args, int *);

    s_tmp = emalloc(1 + MAX_LENGTH_OF_LONG + 1 + 1);
    sprintf(s_tmp, "#%d ", (*num)++);
    _DD_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
    efree(s_tmp);
    if (zend_hash_find(ht, "file", sizeof("file"), (void **)&file) == SUCCESS) {
        if (Z_TYPE_PP(file) != IS_STRING) {
            ddtrace_log_debug("serializer stack trace: Function name is not a string");
            _DD_TRACE_APPEND_STR("[unknown function]");
        } else {
            if (zend_hash_find(ht, "line", sizeof("line"), (void **)&tmp) == SUCCESS) {
                if (Z_TYPE_PP(tmp) == IS_LONG) {
                    line = Z_LVAL_PP(tmp);
                } else {
                    ddtrace_log_debug("serializer stack trace: Line is not a long");
                    line = 0;
                }
            } else {
                line = 0;
            }
            s_tmp = emalloc(Z_STRLEN_PP(file) + MAX_LENGTH_OF_LONG + 4 + 1);
            sprintf(s_tmp, "%s(%ld): ", Z_STRVAL_PP(file), line);
            _DD_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
            efree(s_tmp);
        }
    } else {
        _DD_TRACE_APPEND_STR("[internal function]: ");
    }

    _DD_TRACE_APPEND_KEY("class");
    _DD_TRACE_APPEND_KEY("type");
    _DD_TRACE_APPEND_KEY("function");

    /* We intentionally do not show any arguments, not even an ellipsis if there
     * are arguments; this is for consistency with PHP 7 where there is an INI
     * setting called zend.exception_ignore_args that prevents "args" from being
     * generated.
     */
    _DD_TRACE_APPEND_STR("()\n");

    return ZEND_HASH_APPLY_KEEP;
}

/* This is modelled after getTraceAsString in PHP 5.4
 * @see https://github.com/php/php-src/blob/PHP-5.4/Zend/zend_exceptions.c#L607-L635
 */
static void _serialize_stack_trace(zval *meta, zval *trace TSRMLS_DC) {
    char *res, **str, *s_tmp;
    int res_len = 0, *len = &res_len, num = 0;

    res = estrdup("");
    str = &res;

    zend_hash_apply_with_arguments(Z_ARRVAL_P(trace) TSRMLS_CC, (apply_func_args_t)_trace_string, 3, str, len, &num);

    s_tmp = emalloc(1 + MAX_LENGTH_OF_LONG + 7 + 1);
    sprintf(s_tmp, "#%d {main}", num);
    _DD_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
    efree(s_tmp);

    res[res_len] = '\0';

    add_assoc_string(meta, "error.stack", res, 0);
}

int ddtrace_exception_to_meta(ddtrace_exception_t *exception, void *context,
                              int (*add_tag)(void *context, ddtrace_string key, ddtrace_string value)) {
    UNUSED(exception, context, add_tag);
    return SUCCESS;
}

static void dd_serialize_exception(zval *el, zval *meta, ddtrace_exception_t *exception TSRMLS_DC) {
    zend_uint class_name_len;
    const char *class_name;
    zval *msg = NULL, *stack = NULL;

    if (!exception) {
        return;
    }

    add_assoc_long(el, "error", 1);

    zend_call_method_with_0_params(&exception, Z_OBJCE_P(exception), NULL, "getmessage", &msg);
    if (msg) {
        add_assoc_zval(meta, "error.msg", msg);
    }

    bool use_class_name_for_error_type = true;
    if (instanceof_function(Z_OBJCE_P(exception), ddtrace_ce_fatal_error TSRMLS_CC)) {
        zval *code = NULL;
        zend_call_method_with_0_params(&exception, Z_OBJCE_P(exception), NULL, "getcode", &code);
        if (code) {
            if (Z_TYPE_P(code) == IS_LONG) {
                ddtrace_string error_type;
                switch (Z_LVAL_P(code)) {
                    case E_ERROR:
                        error_type = DDTRACE_STRING_LITERAL("E_ERROR");
                        break;
                    case E_CORE_ERROR:
                        error_type = DDTRACE_STRING_LITERAL("E_CORE_ERROR");
                        break;
                    case E_COMPILE_ERROR:
                        error_type = DDTRACE_STRING_LITERAL("E_COMPILE_ERROR");
                        break;
                    case E_USER_ERROR:
                        error_type = DDTRACE_STRING_LITERAL("E_USER_ERROR");
                        break;
                    default:
                        ZEND_ASSERT(0 && "Unhandled error type in DDTrace\\FatalError; is a fatal error case missing?");
                        error_type = DDTRACE_STRING_LITERAL("{unknown error}");
                }
                add_assoc_stringl(meta, "error.type", error_type.ptr, error_type.len, 1);
                use_class_name_for_error_type = false;
            } else {
                ddtrace_log_debug("Exception was a DDTrace\\FatalError but exception code was not an int");
            }
            zval_ptr_dtor(&code);
        } else {
            ddtrace_log_debug("Failed to fetch exception code of DDTrace\\FatalError");
        }
    }

    if (use_class_name_for_error_type) {
        int needs_copied = zend_get_object_classname(exception, &class_name, &class_name_len TSRMLS_CC);
        /* add_assoc_stringl does not actually mutate the string, but we've either
         * already made a copy, or it will when it duplicates with dup param, so
         * if it did it should still be safe. */
        add_assoc_stringl(meta, "error.type", (char *)class_name, class_name_len, needs_copied);
    }

    /* Note, we use Exception::getTrace() instead of getTraceAsString because
     * function arguments can contain sensitive information. Since we do not
     * have a comprehensive way to know which function arguments are sensitive
     * we will just hide all of them. */
    zend_call_method_with_0_params(&exception, Z_OBJCE_P(exception), NULL, "gettrace", &stack);
    if (stack) {
        _serialize_stack_trace(meta, stack TSRMLS_CC);
        zval_ptr_dtor(&stack);
    }
}

static void _serialize_meta(zval *el, ddtrace_span_fci *span_fci TSRMLS_DC) {
    ddtrace_span_t *span = &span_fci->span;
    zval *meta, *orig_meta = _read_span_property(span->span_data, ZEND_STRL("meta") TSRMLS_CC);
    ALLOC_INIT_ZVAL(meta);
    array_init(meta);
    if (orig_meta && Z_TYPE_P(orig_meta) == IS_ARRAY) {
        int key_type;
        zval **orig_val;
        zval *val_as_string;
        HashPosition pos;
        char *str_key;
        uint str_key_len;
        ulong num_key;
        zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(orig_meta), &pos);
        while (zend_hash_get_current_data_ex(Z_ARRVAL_P(orig_meta), (void **)&orig_val, &pos) == SUCCESS) {
            key_type = zend_hash_get_current_key_ex(Z_ARRVAL_P(orig_meta), &str_key, &str_key_len, &num_key, 0, &pos);
            if (key_type == HASH_KEY_IS_STRING) {
                ALLOC_INIT_ZVAL(val_as_string);
                ddtrace_convert_to_string(val_as_string, *orig_val TSRMLS_CC);
                add_assoc_zval_ex(meta, str_key, str_key_len, val_as_string);
            }
            zend_hash_move_forward_ex(Z_ARRVAL_P(orig_meta), &pos);
        }
    }

    dd_serialize_exception(el, meta, span_fci->exception TSRMLS_CC);
    // zend_hash_exists on PHP 5 needs `sizeof(string)`, not `sizeof(string) - 1`
    if (!span_fci->exception && zend_hash_exists(Z_ARRVAL_P(meta), "error.msg", sizeof("error.msg"))) {
        add_assoc_long(el, "error", 1);
    }
    if (span->parent_id == 0) {
        char pid[MAX_LENGTH_OF_LONG + 1];
        snprintf(pid, sizeof(pid), "%ld", (long)span->pid);
        add_assoc_string(meta, "system.pid", pid, 1);
    }

    // Add meta only if it has elements
    if (zend_hash_num_elements(Z_ARRVAL_P(meta))) {
        add_assoc_zval(el, "meta", meta);
    } else {
        zval_dtor(meta);
        efree(meta);
    }
}

#define ADD_ELEMENT_IF_NOT_NULL(name)                                                        \
    do {                                                                                     \
        zval *prop = _read_span_property(span->span_data, name, sizeof(name) - 1 TSRMLS_CC); \
        zval *prop_as_string;                                                                \
        if (Z_TYPE_P(prop) != IS_NULL) {                                                     \
            ALLOC_INIT_ZVAL(prop_as_string);                                                 \
            ddtrace_convert_to_string(prop_as_string, prop TSRMLS_CC);                       \
            add_assoc_zval(el, name, prop_as_string);                                        \
        }                                                                                    \
    } while (0);

void ddtrace_serialize_span_to_array(ddtrace_span_fci *span_fci, zval *array TSRMLS_DC) {
    ddtrace_span_t *span = &span_fci->span;
    zval *el;
    ALLOC_INIT_ZVAL(el);
    array_init(el);

    add_assoc_long(el, "trace_id", span->trace_id);
    add_assoc_long(el, "span_id", span->span_id);
    if (span->parent_id > 0) {
        add_assoc_long(el, "parent_id", span->parent_id);
    }
    add_assoc_long(el, "start", span->start);
    add_assoc_long(el, "duration", span->duration);

    // SpanData::$name defaults to fully qualified called name (set at span close)
    zval *prop_name = _read_span_property(span->span_data, ZEND_STRL("name") TSRMLS_CC);
    zval *prop_name_as_string = NULL;
    if (Z_TYPE_P(prop_name) != IS_NULL) {
        ALLOC_INIT_ZVAL(prop_name_as_string);
        ddtrace_convert_to_string(prop_name_as_string, prop_name TSRMLS_CC);
        add_assoc_zval(el, "name", prop_name_as_string);
    }

    // SpanData::$resource defaults to SpanData::$name
    zval *prop_resource = _read_span_property(span->span_data, ZEND_STRL("resource") TSRMLS_CC);
    zval *prop_resource_as_string = NULL;
    if (Z_TYPE_P(prop_resource) != IS_NULL) {
        ALLOC_INIT_ZVAL(prop_resource_as_string);
        ddtrace_convert_to_string(prop_resource_as_string, prop_resource TSRMLS_CC);
        add_assoc_zval(el, "resource", prop_resource_as_string);
    } else if (prop_name_as_string) {
        Z_ADDREF_P(prop_name_as_string);
        add_assoc_zval(el, "resource", prop_name_as_string);
    }

    // TODO: SpanData::$service defaults to parent SpanData::$service or DD_SERVICE if root span
    ADD_ELEMENT_IF_NOT_NULL("service");

    // SpanData::$type is optional and defaults to 'custom' at the Agent level
    ADD_ELEMENT_IF_NOT_NULL("type");

    _serialize_meta(el, span_fci TSRMLS_CC);

    zval *metrics = _read_span_property(span->span_data, ZEND_STRL("metrics") TSRMLS_CC);
    if (Z_TYPE_P(metrics) == IS_ARRAY) {
        _add_assoc_zval_copy(el, "metrics", metrics);
    }

    add_next_index_zval(array, el);
}
