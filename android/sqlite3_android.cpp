/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "sqlite3_android"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#include <unistd.h>
#else
#define __attribute(...)
#include <corecrt_io.h>
#endif

#ifdef SQLITE_ENABLE_ICU
#include <unicode/ucol.h>
#include <unicode/uiter.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#endif //SQLITE_ENABLE_ICU
#include <log/log.h>

#include "sqlite3_android.h"
#include "PhoneNumberUtils.h"

#define ENABLE_ANDROID_LOG 0
#define SMALL_BUFFER_SIZE 10
#define PHONE_NUMBER_BUFFER_SIZE 40

#ifdef SQLITE_ENABLE_ICU
static int collate16(void *p, int n1, const void *v1, int n2, const void *v2)
{
    UCollator *coll = (UCollator *) p;
    UCollationResult result = ucol_strcoll(coll, (const UChar *) v1, n1,
                                                 (const UChar *) v2, n2);

    if (result == UCOL_LESS) {
        return -1;
    } else if (result == UCOL_GREATER) {
        return 1;
    } else {
        return 0;
    }
}

static int collate8(void *p, int n1, const void *v1, int n2, const void *v2)
{
    UCollator *coll = (UCollator *) p;
    UCharIterator i1, i2;
    UErrorCode status = U_ZERO_ERROR;

    uiter_setUTF8(&i1, (const char *) v1, n1);
    uiter_setUTF8(&i2, (const char *) v2, n2);

    UCollationResult result = ucol_strcollIter(coll, &i1, &i2, &status);

    if (U_FAILURE(status)) {
//        ALOGE("Collation iterator error: %d\n", status);
    }

    if (result == UCOL_LESS) {
        return -1;
    } else if (result == UCOL_GREATER) {
        return 1;
    } else {
        return 0;
    }
}
#endif // SQLITE_ENABLE_ICU

static void phone_numbers_equal(sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    if (argc != 2 && argc != 3 && argc != 4) {
        sqlite3_result_int(context, 0);
        return;
    }

    char const * num1 = (char const *)sqlite3_value_text(argv[0]);
    char const * num2 = (char const *)sqlite3_value_text(argv[1]);

    bool use_strict = false;
    int min_match = 0;
    if (argc == 3 || argc == 4) {
        use_strict = (sqlite3_value_int(argv[2]) != 0);
        if (!use_strict && argc == 4) {
            min_match = sqlite3_value_int(argv[3]);
        }
    }

    if (num1 == NULL || num2 == NULL) {
        sqlite3_result_null(context);
        return;
    }

    bool equal =
        (use_strict ? android::phone_number_compare_strict(num1, num2)
                    : ((min_match > 0)
                           ? android::phone_number_compare_loose_with_minmatch(
                                 num1, num2, min_match)
                           : android::phone_number_compare_loose(num1, num2)));

    if (equal) {
        sqlite3_result_int(context, 1);
    } else {
        sqlite3_result_int(context, 0);
    }
}

static void phone_number_stripped_reversed(sqlite3_context * context, int argc,
      sqlite3_value ** argv)
{
    if (argc != 1) {
        sqlite3_result_int(context, 0);
        return;
    }

    char const * number = (char const *)sqlite3_value_text(argv[0]);
    if (number == NULL) {
        sqlite3_result_null(context);
        return;
    }

    char out[PHONE_NUMBER_BUFFER_SIZE];
    int outlen = 0;
    android::phone_number_stripped_reversed_inter(number, out, PHONE_NUMBER_BUFFER_SIZE, &outlen);
    sqlite3_result_text(context, (const char*)out, outlen, SQLITE_TRANSIENT);
}


#if ENABLE_ANDROID_LOG
static void android_log(sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    char const * tag = "sqlite_trigger";
    char const * msg = "";
    int msgIndex = 0;

    switch (argc) {
        case 2:
            tag = (char const *)sqlite3_value_text(argv[0]);
            if (tag == NULL) {
                tag = "sqlite_trigger";
            }
            msgIndex = 1;
        case 1:
            msg = (char const *)sqlite3_value_text(argv[msgIndex]);
            if (msg == NULL) {
                msg = "";
            }
            ALOG(LOG_INFO, tag, "%s", msg);
            sqlite3_result_int(context, 1);
            return;

        default:
            sqlite3_result_int(context, 0);
            return;
    }
}
#endif

static void delete_file(sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    if (argc != 1) {
        sqlite3_result_int(context, 0);
        return;
    }

    char const * path = (char const *)sqlite3_value_text(argv[0]);
    // Don't allow ".." in paths
    if (path == NULL || strstr(path, "/../") != NULL) {
        sqlite3_result_null(context);
        return;
    }

    // We only allow deleting files in the EXTERNAL_STORAGE path, or one of the
    // SECONDARY_STORAGE paths
    bool good_path = false;
    char const * external_storage = getenv("EXTERNAL_STORAGE");
    if (external_storage && strncmp(external_storage, path, strlen(external_storage)) == 0) {
        good_path = true;
    } else {
        // check SECONDARY_STORAGE, which should be a colon separated list of paths
        char const * secondary_paths = getenv("SECONDARY_STORAGE");
        while (secondary_paths && secondary_paths[0]) {
            const char* colon = strchr(secondary_paths, ':');
            int length = (colon ? colon - secondary_paths : strlen(secondary_paths));
            if (strncmp(secondary_paths, path, length) == 0) {
                good_path = true;
            }
            secondary_paths += length;
            while (*secondary_paths == ':') secondary_paths++;
        }
    }

    if (!good_path) {
        sqlite3_result_null(context);
        return;
    }

    int err = unlink(path);
    if (err != -1) {
        // No error occured, return true
        sqlite3_result_int(context, 1);
    } else {
        // An error occured, return false
        sqlite3_result_int(context, 0);
    }
}

#ifdef SQLITE_ENABLE_ICU
static void tokenize_auxdata_delete(void * data)
{
    sqlite3_stmt * statement = (sqlite3_stmt *)data;
    sqlite3_finalize(statement);
}

static void base16Encode(char* dest, const char* src, uint32_t size)
{
    static const char * BASE16_TABLE = "0123456789abcdef";
    for (uint32_t i = 0; i < size; i++) {
        char ch = *src++;
        *dest++ = BASE16_TABLE[ (ch & 0xf0) >> 4 ];
        *dest++ = BASE16_TABLE[ (ch & 0x0f)      ];
    }
}

struct SqliteUserData {
    sqlite3 * handle;
    UCollator* collator;
};

#if 0
/**
 * This function is invoked as:
 *
 *  _TOKENIZE('<token_table>', <data_row_id>, <data>, <delimiter>,
 *             <use_token_index>, <data_tag>)
 *
 * If <use_token_index> is omitted, it is treated as 0.
 * If <data_tag> is omitted, it is treated as NULL.
 *
 * It will split <data> on each instance of <delimiter> and insert each token
 * into <token_table>. The following columns in <token_table> are used:
 * token TEXT, source INTEGER, token_index INTEGER, tag (any type)
 * The token_index column is not required if <use_token_index> is 0.
 * The tag column is not required if <data_tag> is NULL.
 *
 * One row is inserted for each token in <data>.
 * In each inserted row, 'source' is <data_row_id>.
 * In the first inserted row, 'token' is the hex collation key of
 * the entire <data> string, and 'token_index' is 0.
 * In each row I (where 1 <= I < N, and N is the number of tokens in <data>)
 * 'token' will be set to the hex collation key of the I:th token (0-based).
 * If <use_token_index> != 0, 'token_index' is set to I.
 * If <data_tag> is not NULL, 'tag' is set to <data_tag>.
 *
 * In other words, there will be one row for the entire string,
 * and one row for each token except the first one.
 *
 * The function returns the number of tokens generated.
 */
static void tokenize(sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    //ALOGD("enter tokenize");
    int err;
    int useTokenIndex = 0;
    int useDataTag = 0;

    if (!(argc >= 4 || argc <= 6)) {
        ALOGE("Tokenize requires 4 to 6 arguments");
        sqlite3_result_null(context);
        return;
    }

    if (argc > 4) {
        useTokenIndex = sqlite3_value_int(argv[4]);
    }

    if (argc > 5) {
        useDataTag = (sqlite3_value_type(argv[5]) != SQLITE_NULL);
    }

    sqlite3 * handle = sqlite3_context_db_handle(context);
    UCollator* collator = (UCollator*)sqlite3_user_data(context);
    char const * tokenTable = (char const *)sqlite3_value_text(argv[0]);
    if (tokenTable == NULL) {
        ALOGE("tokenTable null");
        sqlite3_result_null(context);
        return;
    }

    // Get or create the prepared statement for the insertions
    sqlite3_stmt * statement = (sqlite3_stmt *)sqlite3_get_auxdata(context, 0);
    if (!statement) {
        char const * tokenIndexCol = useTokenIndex ? ", token_index" : "";
        char const * tokenIndexParam = useTokenIndex ? ", ?" : "";
        char const * dataTagCol = useDataTag ? ", tag" : "";
        char const * dataTagParam = useDataTag ? ", ?" : "";
        char * sql = sqlite3_mprintf("INSERT INTO %s (token, source%s%s) VALUES (?, ?%s%s);",
                tokenTable, tokenIndexCol, dataTagCol, tokenIndexParam, dataTagParam);
        err = sqlite3_prepare_v2(handle, sql, -1, &statement, NULL);
        sqlite3_free(sql);
        if (err) {
            ALOGE("prepare failed");
            sqlite3_result_null(context);
            return;
        }
        // This binds the statement to the table it was compiled against, which is argv[0].
        // If this function is ever called with a different table the finalizer will be called
        // and sqlite3_get_auxdata() will return null above, forcing a recompile for the new table.
        sqlite3_set_auxdata(context, 0, statement, tokenize_auxdata_delete);
    } else {
        // Reset the cached statement so that binding the row ID will work properly
        sqlite3_reset(statement);
    }

    // Bind the row ID of the source row
    int64_t rowID = sqlite3_value_int64(argv[1]);
    err = sqlite3_bind_int64(statement, 2, rowID);
    if (err != SQLITE_OK) {
        ALOGE("bind failed");
        sqlite3_result_null(context);
        return;
    }

    // Bind <data_tag> to the tag column
    if (useDataTag) {
        int dataTagParamIndex = useTokenIndex ? 4 : 3;
        err = sqlite3_bind_value(statement, dataTagParamIndex, argv[5]);
        if (err != SQLITE_OK) {
            ALOGE("bind failed");
            sqlite3_result_null(context);
            return;
        }
    }

    // Get the raw bytes for the string to tokenize
    // the string will be modified by following code
    // however, sqlite did not reuse the string, so it is safe to not dup it
    UChar * origData = (UChar *)sqlite3_value_text16(argv[2]);
    if (origData == NULL) {
        sqlite3_result_null(context);
        return;
    }

    // Get the raw bytes for the delimiter
    const UChar * delim = (const UChar *)sqlite3_value_text16(argv[3]);
    if (delim == NULL) {
        ALOGE("can't get delimiter");
        sqlite3_result_null(context);
        return;
    }

    UChar * token = NULL;
    UChar *state;
    int numTokens = 0;

    do {
        if (numTokens == 0) {
            token = origData;
        }

        // Reset the program so we can use it to perform the insert
        sqlite3_reset(statement);
        UErrorCode status = U_ZERO_ERROR;
        char keybuf[1024];
        uint32_t result = ucol_getSortKey(collator, token, -1, (uint8_t*)keybuf, sizeof(keybuf)-1);
        if (result > sizeof(keybuf)) {
            // TODO allocate memory for this super big string
            ALOGE("ucol_getSortKey needs bigger buffer %d", result);
            break;
        }
        uint32_t keysize = result-1;
        uint32_t base16Size = keysize*2;
        char *base16buf = (char*)malloc(base16Size);
        base16Encode(base16buf, keybuf, keysize);
        err = sqlite3_bind_text(statement, 1, base16buf, base16Size, SQLITE_STATIC);

        if (err != SQLITE_OK) {
            ALOGE(" sqlite3_bind_text16 error %d", err);
            free(base16buf);
            break;
        }

        if (useTokenIndex) {
            err = sqlite3_bind_int(statement, 3, numTokens);
            if (err != SQLITE_OK) {
                ALOGE(" sqlite3_bind_int error %d", err);
                free(base16buf);
                break;
            }
        }

        err = sqlite3_step(statement);
        free(base16buf);

        if (err != SQLITE_DONE) {
            ALOGE(" sqlite3_step error %d", err);
            break;
        }
        numTokens++;
        if (numTokens == 1) {
            // first call
            u_strtok_r(origData, delim, &state);
        }
    } while ((token = u_strtok_r(NULL, delim, &state)) != NULL);
    sqlite3_result_int(context, numTokens);
}
#endif

static void localized_collator_dtor(UCollator* collator)
{
    ucol_close(collator);
}

#define LOCALIZED_COLLATOR_NAME "LOCALIZED"

// This collator may be removed in the near future, so you MUST not use now.
#define PHONEBOOK_COLLATOR_NAME "PHONEBOOK"

#endif // SQLITE_ENABLE_ICU

extern "C" int register_localized_collators(sqlite3* handle __attribute((unused)),
                                            const char* systemLocale __attribute((unused)),
                                            int utf16Storage __attribute((unused)))
{
// This function is no-op for the VNDK, but should exist in case when some vendor
// module has a reference to this function.
#ifdef SQLITE_ENABLE_ICU
    UErrorCode status = U_ZERO_ERROR;
    UCollator* collator = ucol_open(systemLocale, &status);
    if (U_FAILURE(status)) {
        return -1;
    }

    ucol_setAttribute(collator, UCOL_STRENGTH, UCOL_PRIMARY, &status);
    if (U_FAILURE(status)) {
        return -1;
    }

    int err;
    if (utf16Storage) {
        err = sqlite3_create_collation_v2(handle, LOCALIZED_COLLATOR_NAME, SQLITE_UTF16, collator,
                collate16, (void(*)(void*))localized_collator_dtor);
    } else {
        err = sqlite3_create_collation_v2(handle, LOCALIZED_COLLATOR_NAME, SQLITE_UTF8, collator,
                collate8, (void(*)(void*))localized_collator_dtor);
    }

    if (err != SQLITE_OK) {
        return err;
    }

#if 0
    // Register the _TOKENIZE function
    err = sqlite3_create_function(handle, "_TOKENIZE", 4, SQLITE_UTF16, collator, tokenize, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }
    err = sqlite3_create_function(handle, "_TOKENIZE", 5, SQLITE_UTF16, collator, tokenize, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }
    err = sqlite3_create_function(handle, "_TOKENIZE", 6, SQLITE_UTF16, collator, tokenize, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }
#endif

    //// PHONEBOOK_COLLATOR
    status = U_ZERO_ERROR;
    collator = ucol_open(systemLocale, &status);
    if (U_FAILURE(status)) {
        return -1;
    }

    status = U_ZERO_ERROR;
    ucol_setAttribute(collator, UCOL_STRENGTH, UCOL_PRIMARY, &status);
    if (U_FAILURE(status)) {
        return -1;
    }

    if (utf16Storage) {
        err = sqlite3_create_collation_v2(handle, PHONEBOOK_COLLATOR_NAME, SQLITE_UTF16, collator,
                collate16, (void(*)(void*))localized_collator_dtor);
    } else {
        err = sqlite3_create_collation_v2(handle, PHONEBOOK_COLLATOR_NAME, SQLITE_UTF8, collator,
                collate8, (void(*)(void*))localized_collator_dtor);
    }

    if (err != SQLITE_OK) {
        return err;
    }
    //// PHONEBOOK_COLLATOR
#endif //SQLITE_ENABLE_ICU

    return SQLITE_OK;
}


extern "C" int register_android_functions(sqlite3 * handle, int utf16Storage __attribute((unused)))
{
    int err;
#ifdef SQLITE_ENABLE_ICU
    UErrorCode status = U_ZERO_ERROR;

    UCollator * collator = ucol_open(NULL, &status);
    if (U_FAILURE(status)) {
        return -1;
    }

    if (utf16Storage) {
        // Note that text should be stored as UTF-16
        err = sqlite3_exec(handle, "PRAGMA encoding = 'UTF-16'", 0, 0, 0);
        if (err != SQLITE_OK) {
            return err;
        }

        // Register the UNICODE collation
        err = sqlite3_create_collation_v2(handle, "UNICODE", SQLITE_UTF16, collator, collate16,
                (void(*)(void*))localized_collator_dtor);
    } else {
        err = sqlite3_create_collation_v2(handle, "UNICODE", SQLITE_UTF8, collator, collate8,
                (void(*)(void*))localized_collator_dtor);
    }

    if (err != SQLITE_OK) {
        return err;
    }
#endif // SQLITE_ENABLE_ICU

    // Register the PHONE_NUM_EQUALS function
    err = sqlite3_create_function(
        handle, "PHONE_NUMBERS_EQUAL", 2,
        SQLITE_UTF8, NULL, phone_numbers_equal, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }

    // Register the PHONE_NUM_EQUALS function with an additional argument "use_strict"
    err = sqlite3_create_function(
        handle, "PHONE_NUMBERS_EQUAL", 3,
        SQLITE_UTF8, NULL, phone_numbers_equal, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }

    // Register the PHONE_NUM_EQUALS function with additional arguments "use_strict" and "min_match"
    err = sqlite3_create_function(
        handle, "PHONE_NUMBERS_EQUAL", 4,
        SQLITE_UTF8, NULL, phone_numbers_equal, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }

    // Register the _DELETE_FILE function
    err = sqlite3_create_function(handle, "_DELETE_FILE", 1, SQLITE_UTF8, NULL, delete_file, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }

#if ENABLE_ANDROID_LOG
    // Register the _LOG function
    err = sqlite3_create_function(handle, "_LOG", 1, SQLITE_UTF8, NULL, android_log, NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }
#endif

    // Register the _PHONE_NUMBER_STRIPPED_REVERSED function, which imitates
    // PhoneNumberUtils.getStrippedReversed.  This function is used by
    // packages/providers/ContactsProvider/src/com/android/providers/contacts/LegacyApiSupport.java
    // to provide compatibility with Android 1.6 and earlier.
    err = sqlite3_create_function(handle,
        "_PHONE_NUMBER_STRIPPED_REVERSED",
        1, SQLITE_UTF8, NULL,
        phone_number_stripped_reversed,
        NULL, NULL);
    if (err != SQLITE_OK) {
        return err;
    }

    return SQLITE_OK;
}
