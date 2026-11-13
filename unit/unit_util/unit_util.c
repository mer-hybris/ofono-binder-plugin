/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "binder_util.h"

#include <ofono/log.h>
#include <ofono/misc.h>
#include <ofono/radio-settings.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_log.h>

#include <string.h>

/*==========================================================================*
 * Stubs
 *
 * libgbinder has no public way to make a reader out of raw bytes, so
 * the two reader calls binder_read_array_count() makes are replaced
 * here by ones that walk a plain int32 array. The executable's
 * definitions take precedence over libgbinder's.
 *==========================================================================*/

typedef struct test_reader {
    const gint32* ptr;
    const gint32* end;
} TestReader;

G_STATIC_ASSERT(sizeof(TestReader) <= sizeof(GBinderReader));

#define TEST_READER(r) ((TestReader*)(r))

static
void
test_reader_init(
    GBinderReader* reader,
    const gint32* data,
    gsize count)
{
    memset(reader, 0, sizeof(*reader));
    TEST_READER(reader)->ptr = data;
    TEST_READER(reader)->end = data + count;
}

gboolean
gbinder_reader_read_int32(
    GBinderReader* reader,
    gint32* value)
{
    TestReader* r = TEST_READER(reader);

    if (r->ptr < r->end) {
        *value = *r->ptr++;
        return TRUE;
    }
    return FALSE;
}

gsize
gbinder_reader_bytes_remaining(
    const GBinderReader* reader)
{
    const TestReader* r = (const TestReader*)reader;

    return (r->end - r->ptr) * sizeof(gint32);
}

/*
 * The writer only needs to hand out memory which it would free together
 * with the parcel. Keep track of it here and free it after each test.
 */
static GSList* test_writer_blocks = NULL;

void*
gbinder_writer_malloc(
    GBinderWriter* writer,
    gsize size)
{
    void* ptr = g_malloc(size);

    test_writer_blocks = g_slist_prepend(test_writer_blocks, ptr);
    return ptr;
}

static
void
test_writer_free(
    void)
{
    g_slist_free_full(test_writer_blocks, g_free);
    test_writer_blocks = NULL;
}

void ofono_dbg(const struct ofono_debug_desc* desc, const char* format, ...) {}
void ofono_warn(const char* format, ...) {}
void ofono_encode_hex(const void* in, unsigned int len, char* out) {}
enum ofono_radio_access_mode ofono_radio_access_max_mode(
    enum ofono_radio_access_mode mask) { return mask; }

/*==========================================================================*
 * array_count
 *==========================================================================*/

static
void
test_array_count(
    void)
{
    /* Length, then the minimum (one int32) per element */
    static const gint32 empty[] = { 0 };
    static const gint32 null_array[] = { -1 };
    static const gint32 two[] = { 2, 0, 0 };
    static const gint32 two_short[] = { 2, 0 };
    static const gint32 negative[] = { -2, 0, 0 };
    static const gint32 huge[] = { G_MAXINT32, 0, 0 };
    GBinderReader reader;
    guint count;

    /* Nothing to read */
    test_reader_init(&reader, empty, 0);
    count = 42;
    g_assert(!binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);

    test_reader_init(&reader, empty, G_N_ELEMENTS(empty));
    count = 42;
    g_assert(binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);

    /* AIDL null array */
    test_reader_init(&reader, null_array, G_N_ELEMENTS(null_array));
    count = 42;
    g_assert(binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);

    /* Just fits */
    test_reader_init(&reader, two, G_N_ELEMENTS(two));
    g_assert(binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,2);

    /* One element short */
    test_reader_init(&reader, two_short, G_N_ELEMENTS(two_short));
    count = 42;
    g_assert(!binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);

    /* Negative other than -1 */
    test_reader_init(&reader, negative, G_N_ELEMENTS(negative));
    count = 42;
    g_assert(!binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);

    /* Way more than the parcel holds */
    test_reader_init(&reader, huge, G_N_ELEMENTS(huge));
    count = 42;
    g_assert(!binder_read_array_count(&reader, &count));
    g_assert_cmpuint(count, == ,0);
}

/*==========================================================================*
 * copy_hidl_string
 *==========================================================================*/

static
void
test_copy_hidl_string(
    void)
{
    /* No NULL terminator after the first 3 characters */
    static const char abc[] = { 'a', 'b', 'c', 'X' };
    GBinderWriter writer;
    GBinderHidlString str;

    memset(&writer, 0, sizeof(writer));

    binder_copy_hidl_string(&writer, &str, "test");
    g_assert_cmpuint(str.len, == ,4);
    g_assert_cmpstr(str.data.str, == ,"test");
    g_assert(str.owns_buffer);

    binder_copy_hidl_string(&writer, &str, NULL);
    g_assert_cmpuint(str.len, == ,0);
    g_assert_cmpstr(str.data.str, == ,"");

    /* Explicit length must be honored and the copy terminated */
    binder_copy_hidl_string_len(&writer, &str, abc, 3);
    g_assert_cmpuint(str.len, == ,3);
    g_assert_cmpstr(str.data.str, == ,"abc");

    binder_copy_hidl_string_len(&writer, &str, "test", 2);
    g_assert_cmpuint(str.len, == ,2);
    g_assert_cmpstr(str.data.str, == ,"te");

    /* Negative length means NULL-terminated */
    binder_copy_hidl_string_len(&writer, &str, "test", -1);
    g_assert_cmpuint(str.len, == ,4);
    g_assert_cmpstr(str.data.str, == ,"test");

    binder_copy_hidl_string_len(&writer, &str, "test", 0);
    g_assert_cmpuint(str.len, == ,0);
    g_assert_cmpstr(str.data.str, == ,"");

    binder_copy_hidl_string_len(&writer, &str, NULL, 5);
    g_assert_cmpuint(str.len, == ,0);
    g_assert_cmpstr(str.data.str, == ,"");

    test_writer_free();
}

static
void
test_strv_from_hidl_link_address_vec(
    void)
{
    static const char v4[] = "192.0.2.1/24";
    static const char v6[] = "2001:db8::1/64";
    RadioLinkAddress addrs[2];
    GBinderHidlVec vec;
    char** strv;

    memset(addrs, 0, sizeof(addrs));
    addrs[0].address.data.str = v4;
    addrs[0].address.len = sizeof(v4) - 1;
    addrs[1].address.data.str = v6;
    addrs[1].address.len = sizeof(v6) - 1;

    memset(&vec, 0, sizeof(vec));
    vec.data.ptr = addrs;
    vec.count = G_N_ELEMENTS(addrs);

    /* Every entry, not just the first */
    strv = binder_strv_from_hidl_link_address_vec(&vec);
    g_assert_cmpuint(g_strv_length(strv), == ,2);
    g_assert_cmpstr(strv[0], == ,v4);
    g_assert_cmpstr(strv[1], == ,v6);
    g_strfreev(strv);

    /* Missing address string */
    addrs[1].address.data.str = NULL;
    addrs[1].address.len = 0;
    strv = binder_strv_from_hidl_link_address_vec(&vec);
    g_assert_cmpstr(strv[1], == ,"");
    g_strfreev(strv);

    vec.count = 0;
    vec.data.ptr = NULL;
    strv = binder_strv_from_hidl_link_address_vec(&vec);
    g_assert(strv);
    g_assert(!strv[0]);
    g_strfreev(strv);

    g_assert(!binder_strv_from_hidl_link_address_vec(NULL));
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/util/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("array_count"), test_array_count);
    g_test_add_func(TEST_("copy_hidl_string"), test_copy_hidl_string);
    g_test_add_func(TEST_("strv_from_hidl_link_address_vec"),
        test_strv_from_hidl_link_address_vec);

    gutil_log_default.level = g_test_verbose() ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
    gutil_log_timestamp = FALSE;

    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
