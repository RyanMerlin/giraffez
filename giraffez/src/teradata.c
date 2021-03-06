/*
 * Copyright 2016 Capital One Services, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common.h"
#include "encoder.h"

#include "teradata.h"

// Teradata CLIv2
#include <coperr.h>
#include <dbcarea.h>
#include <parcel.h>

// CLIv2 control flow exceptions
PyObject *EndStatementError;
PyObject *EndStatementInfoError;
PyObject *EndRequestError;


TeradataCursor* cursor_new(const char *command) {
    TeradataCursor *cursor;
    cursor = (TeradataCursor*)malloc(sizeof(TeradataCursor));
    cursor->rc = 0;
    cursor->err = NULL;
    cursor->rowcount = -1;
    cursor->req_proc_opt = 'B';
    cursor->command = strdup(command);
    return cursor;
}

void cursor_free(TeradataCursor *cursor) {
    if (cursor == NULL) {
        return;
    }
    if (cursor->err != NULL) {
        free(cursor->err);
        cursor->err = NULL;
    }
    if (cursor->command != NULL) {
        free(cursor->command);
        cursor->command = NULL;
    }
    free(cursor);
    cursor = NULL;
}

TeradataConnection* teradata_new() {
    TeradataConnection *conn;
    conn = (TeradataConnection*)malloc(sizeof(TeradataConnection));
    conn->result = 0;
    conn->connected = NOT_CONNECTED;
    conn->request_status = REQUEST_CLOSED;
    conn->dbc = (dbcarea_t*)malloc(sizeof(dbcarea_t));
    conn->dbc->total_len = sizeof(*conn->dbc);
    return conn;
}

uint16_t teradata_fetch_parcel(TeradataConnection *conn) {
    Py_BEGIN_ALLOW_THREADS
    DBCHCL(&conn->result, conn->cnta, conn->dbc);
    Py_END_ALLOW_THREADS
    return conn->result;
}

uint16_t teradata_end_request(TeradataConnection *conn) {
    if (conn->request_status == REQUEST_CLOSED) {
        return conn->result;
    }
    conn->dbc->i_sess_id = conn->dbc->o_sess_id;
    conn->dbc->i_req_id = conn->dbc->o_req_id;
    conn->dbc->func = DBFERQ;
    DBCHCL(&conn->result, conn->cnta, conn->dbc);
    if (conn->result == OK) {
        conn->request_status = REQUEST_CLOSED;
    }
    return conn->result;
}

void teradata_free(TeradataConnection *conn) {
    if (conn != NULL) {
        if (conn->dbc != NULL) {
            free(conn->dbc);
        }
        free(conn);
        conn = NULL;
    }
}

PyObject* teradata_close(TeradataConnection *conn) {
    if (conn == NULL) {
        Py_RETURN_NONE;
    }
    if (teradata_end_request(conn) != OK) {
        PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    if (conn->connected == CONNECTED) {
        conn->dbc->func = DBFDSC;
        Py_BEGIN_ALLOW_THREADS
        DBCHCL(&conn->result, conn->cnta, conn->dbc);
        Py_END_ALLOW_THREADS
    }
    // TODO: this presumably cleans up some of the global variables
    // used by CLIv2 and if done when working with multiple sessions
    // concurrently will cause a segfault. It is possible that
    // running it at all isn't really necessary and this can get cleaned
    // up with the process dying. Alternatives might be using atexit
    // calls to clean it up formally before the process exits and
    // also providing a function that can be called should it be necessary
    // for someone to clean this up (for like long running processes).
    // DBCHCLN(&conn->result, conn->cnta);
    teradata_free(conn);
    Py_RETURN_NONE;
}

TeradataConnection* teradata_connect(const char *host, const char *username,
        const char *password, const char *logon_mech, const char *logon_mech_data) {
    int status;
    TeradataConnection *conn;
    conn = teradata_new();
    DBCHINI(&conn->result, conn->cnta, conn->dbc);
    if (conn->result != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[init]: %s", conn->result, conn->dbc->msg_text);
        teradata_free(conn);
        return NULL;
    }
    conn->dbc->change_opts = 'Y';
    conn->dbc->resp_mode = 'I';
    conn->dbc->use_presence_bits = 'N';
    conn->dbc->keep_resp = 'N';
    conn->dbc->wait_across_crash = 'N';
    conn->dbc->tell_about_crash = 'Y';
    conn->dbc->loc_mode = 'Y';
    conn->dbc->var_len_req = 'N';
    conn->dbc->var_len_fetch = 'N';
    conn->dbc->save_resp_buf = 'N';
    conn->dbc->two_resp_bufs = 'Y';
    conn->dbc->ret_time = 'N';
    conn->dbc->parcel_mode = 'Y';
    conn->dbc->wait_for_resp = 'Y';
    conn->dbc->req_proc_opt = 'B';
    conn->dbc->return_statement_info = 'Y';
    conn->dbc->req_buf_len = 65535;
    conn->dbc->maximum_parcel = 'H';
    conn->dbc->max_decimal_returned = 38;
    conn->dbc->charset_type = 'N';

    // The date is set explicitly to only use the Teradata format.  The
    // Teradata CLIv2 documentation indicates that the Teradata format will
    // always be available, but the other date type, ANSI, may not be
    // available.  Since the Teradata date format (integer date) is the
    // preferred date format anyway, this can be set explicitly to safely
    // address issues like Teradata server configurations where the default
    // date format is set to ANSI.
    conn->dbc->date_form = 'T';
    conn->dbc->tx_semantics = 'T';
    conn->dbc->consider_APH_resps = 'Y';
    snprintf(conn->session_charset, sizeof(conn->session_charset), "%-*s",
        (int)(sizeof(conn->session_charset)-strlen(TERADATA_CHARSET)), TERADATA_CHARSET);
    conn->dbc->inter_ptr = conn->session_charset;
    sprintf(conn->logonstr, "%s/%s,%s", host, username, password);
    conn->dbc->logon_ptr = conn->logonstr;
    conn->dbc->logon_len = (UInt32)strlen(conn->logonstr);
    conn->dbc->func = DBFCON;
    if (logon_mech != NULL) {
        snprintf(conn->dbc->logmech_name, sizeof(conn->dbc->logmech_name), "%-*s",
            (int)(sizeof(conn->dbc->logmech_name)), logon_mech); // @RYANMERLIN removed -strlen(logon_mech) -- https://github.com/capitalone/giraffez/issues/52
        if (logon_mech_data != NULL) {
            sprintf(conn->dbc->logmech_data_ptr, "%s", logon_mech_data);
            conn->dbc->logmech_data_len = (UInt32)strlen(conn->dbc->logmech_data_ptr);
        }
    }
    DBCHCL(&conn->result, conn->cnta, conn->dbc);
    if (conn->result != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[connect]: %s", conn->result, conn->dbc->msg_text);
        teradata_free(conn);
        return NULL;
    }
    conn->dbc->i_sess_id = conn->dbc->o_sess_id;
    conn->dbc->i_req_id = conn->dbc->o_req_id;
    conn->dbc->func = DBFFET;
    if ((status = teradata_fetch_parcel(conn)) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[fetch]: %s", conn->result, conn->dbc->msg_text);
        teradata_free(conn);
        return NULL;
    } else {
        struct CliErrorType *error;
        struct CliFailureType *failure;
        switch (conn->dbc->fet_parcel_flavor) {
            case PclERROR:
                error = (struct CliErrorType*)conn->dbc->fet_data_ptr;
                if (error->Code == TD_ERROR_INVALID_USER) {
                    PyErr_Format(InvalidCredentialsError, "%d: %s", error->Code, error->Msg);
                } else {
                    PyErr_Format(TeradataError, "%d: %s", error->Code, error->Msg);
                }
                teradata_free(conn);
                return NULL;
            case PclFAILURE:
                failure = (struct CliFailureType*)conn->dbc->fet_data_ptr;
                if (failure->Code == TD_ERROR_INVALID_USER) {
                    PyErr_Format(InvalidCredentialsError, "%d: %s", failure->Code, failure->Msg);
                } else {
                    PyErr_Format(TeradataError, "%d: %s", failure->Code, failure->Msg);
                }
                teradata_free(conn);
                return NULL;
        }
    }
    if (teradata_end_request(conn) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[end_request]: %s", conn->result, conn->dbc->msg_text);
        teradata_free(conn);
        return NULL;
    }
    conn->connected = CONNECTED;
    return conn;
}

PyObject* teradata_check_error(TeradataConnection *conn, TeradataCursor *cursor) {
    if (conn->result == TD_ERROR_REQUEST_EXHAUSTED && conn->connected == CONNECTED) {
        if (teradata_end_request(conn) != OK) {
            if (cursor != NULL) {
                cursor->rc = conn->result;
            }
            PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
            return NULL;
        }
    } else if (conn->result != OK) {
        if (cursor != NULL) {
            cursor->rc = conn->result;
        }
        PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    if (cursor != NULL) {
        cursor->rc = conn->result;
    }
    Py_RETURN_NONE;
}

PyObject* teradata_execute(TeradataConnection *conn, TeradataEncoder *encoder, TeradataCursor *cursor) {
    size_t count;
    conn->dbc->req_proc_opt = cursor->req_proc_opt;
    conn->dbc->req_ptr = cursor->command;
    conn->dbc->req_len = (UInt32)strlen(cursor->command);
    conn->dbc->func = DBFIRQ;
    Py_BEGIN_ALLOW_THREADS
    DBCHCL(&conn->result, conn->cnta, conn->dbc);
    Py_END_ALLOW_THREADS
    if (conn->result == OK) {
        conn->request_status = REQUEST_OPEN;
    } else {
        PyErr_Format(TeradataError, "%d: CLIv2[execute_init]: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    conn->dbc->i_sess_id = conn->dbc->o_sess_id;
    conn->dbc->i_req_id = conn->dbc->o_req_id;
    conn->dbc->func = DBFFET;
    count = 0;
    while (teradata_fetch_parcel(conn) == OK && count < MAX_PARCEL_ATTEMPTS) {
        if (teradata_handle_parcel_status(cursor, conn->dbc->fet_parcel_flavor, (unsigned char**)&conn->dbc->fet_data_ptr, conn->dbc->fet_ret_data_len) == NULL) {
            return NULL;
        }
        if (teradata_handle_parcel_state(encoder, conn->dbc->fet_parcel_flavor, (unsigned char**)&conn->dbc->fet_data_ptr, conn->dbc->fet_ret_data_len) == NULL) {
            return NULL;
        }
        if (encoder != NULL && encoder->Columns != NULL) {
            Py_RETURN_NONE;
        }
        count++;
    }
    return teradata_check_error(conn, cursor);
}

PyObject* teradata_fetch_all(TeradataConnection *conn, TeradataEncoder *encoder, TeradataCursor *cursor) {
    while (teradata_fetch_parcel(conn) == OK) {
        if (teradata_handle_parcel_status(cursor, conn->dbc->fet_parcel_flavor,
                    (unsigned char**)&conn->dbc->fet_data_ptr,
                    conn->dbc->fet_ret_data_len) == NULL) {
            return NULL;
        }
    }
    return teradata_check_error(conn, cursor);
}

PyObject* teradata_fetch_row(TeradataConnection *conn, TeradataEncoder *encoder, TeradataCursor *cursor) {
    PyObject *row = NULL;
    while (teradata_fetch_parcel(conn) == OK) {
        if ((row = teradata_handle_record(encoder, cursor, conn->dbc->fet_parcel_flavor,
                (unsigned char**)&conn->dbc->fet_data_ptr,
                conn->dbc->fet_ret_data_len)) == NULL) {
            return NULL;
        } else if (row != Py_None) {
            return row;
        }
    }
    return teradata_check_error(conn, NULL);
}

TeradataErr* teradata_error(int code, char *msg) {
    TeradataErr *err;
    err = (TeradataErr*)malloc(sizeof(TeradataErr));
    err->Code = code;
    err->Msg = strdup(msg);
    return err;
}

void teradata_error_free(TeradataErr *err) {
    if (err != NULL) {
        if (err->Msg != NULL) {
            free(err->Msg);
        }
        free(err);
    }
}

PyObject* teradata_handle_parcel_status(TeradataCursor *cursor, const uint32_t parcel_t, unsigned char **data, const uint32_t length) {
    struct CliSuccessType *success;
    struct CliErrorType *error;
    struct CliFailureType *failure;
    switch (parcel_t) {
        case PclSUCCESS:
            if (cursor != NULL) {
                // TODO: The IBM 370 mainframes return an integer (PclUInt32)
                // instead of a char array, and should be handled.
                success = (struct CliSuccessType*)*data;
                *data += sizeof(success);
                uint32_t count = 0;
                count = (unsigned char)success->ActivityCount[0];
                count |= (unsigned char)success->ActivityCount[1] << 8;
                count |= (unsigned char)success->ActivityCount[2] << 16;
                count |= (unsigned char)success->ActivityCount[3] << 24;
                cursor->rowcount = (int64_t)count;
            }
            break;
        case PclFAILURE:
            failure = (struct CliFailureType*)*data;
            if (cursor != NULL) {
                cursor->err = teradata_error(failure->Code, failure->Msg);;
            }
            if (failure->Code == TD_ERROR_INVALID_USER) {
                PyErr_Format(InvalidCredentialsError, "%d: %s", failure->Code, failure->Msg);
                return NULL;
            }
            PyErr_Format(TeradataError, "%d: %s", failure->Code, failure->Msg);
            return NULL;
        case PclERROR:
            error = (struct CliErrorType*)*data;
            if (cursor != NULL) {
                cursor->err = teradata_error(error->Code, error->Msg);
            }
            PyErr_Format(TeradataError, "%d: %s", error->Code, error->Msg);
            return NULL;
    }
    Py_RETURN_NONE;
}

PyObject* teradata_handle_parcel_state(TeradataEncoder *encoder, const uint32_t parcel_t, unsigned char **data, const uint32_t length) {
    switch (parcel_t) {
        case PclSTATEMENTINFO:
            encoder_clear(encoder);
            encoder->Columns = encoder->UnpackStmtInfoFunc(data, length);
            break;
        case PclSTATEMENTINFOEND:
            PyErr_SetNone(EndStatementInfoError);
            return NULL;
        case PclENDSTATEMENT:
            PyErr_SetNone(EndStatementError);
            return NULL;
        case PclENDREQUEST:
            PyErr_SetNone(EndRequestError);
            return NULL;
    }
    Py_RETURN_NONE;
}

PyObject* teradata_handle_parcel_record(TeradataEncoder *encoder, const uint32_t parcel_t, unsigned char **data, const uint32_t length) {
    PyGILState_STATE state;
    PyObject *row = NULL;
    if (parcel_t == PclRECORD) {
        state = PyGILState_Ensure();
        if ((row = encoder->UnpackRowFunc(encoder, data, length)) == NULL) {
            return NULL;
        }
        PyGILState_Release(state);
        return row;
    }
    Py_RETURN_NONE;
}

PyObject* teradata_handle_record(TeradataEncoder *encoder, TeradataCursor *cursor, const uint32_t parcel_t, unsigned char **data, const uint32_t length) {
    if (teradata_handle_parcel_status(cursor, parcel_t, data, length) == NULL) {
        return NULL;
    }
    if (teradata_handle_parcel_state(encoder, parcel_t, data, length) == NULL) {
        return NULL;
    }
    return teradata_handle_parcel_record(encoder, parcel_t, data, length);
}

uint16_t teradata_type_to_tpt_type(uint16_t t) {
    switch (t) {
        case BLOB_NN:
        case BLOB_N:
        case BLOB_AS_DEFERRED_NN:
        case BLOB_AS_DEFERRED_N:
        case BLOB_AS_LOCATOR_NN:
        case BLOB_AS_LOCATOR_N:
            return TD_BLOB;
        case BLOB_AS_DEFERRED_NAME_NN:
        case BLOB_AS_DEFERRED_NAME_N:
            return TD_BLOB_AS_DEFERRED_BY_NAME;
        case CLOB_NN:
        case CLOB_N:
            return TD_CLOB;
        case CLOB_AS_DEFERRED_NN:
        case CLOB_AS_DEFERRED_N:
            return TD_CLOB_AS_DEFERRED_BY_NAME;
        case CLOB_AS_LOCATOR_NN:
        case CLOB_AS_LOCATOR_N:
            return TD_CLOB;
        case UDT_NN:
        case UDT_N:
        case DISTINCT_UDT_NN:
        case DISTINCT_UDT_N:
        case STRUCT_UDT_NN:
        case STRUCT_UDT_N:
            return TD_CHAR;
        case VARCHAR_NN:
        case VARCHAR_N:
            return TD_VARCHAR;
        case CHAR_NN:
        case CHAR_N:
            return TD_CHAR;
        case LONG_VARCHAR_NN:
        case LONG_VARCHAR_N:
            return TD_LONGVARCHAR;
        case VARGRAPHIC_NN:
        case VARGRAPHIC_N:
            return TD_VARGRAPHIC;
        case GRAPHIC_NN:
        case GRAPHIC_N:
            return TD_GRAPHIC;
        case LONG_VARGRAPHIC_NN:
        case LONG_VARGRAPHIC_N:
            return TD_LONGVARGRAPHIC;
        case FLOAT_NN:
        case FLOAT_N:
            return TD_FLOAT;
        case DECIMAL_NN:
        case DECIMAL_N:
            return TD_DECIMAL;
        case INTEGER_NN:
        case INTEGER_N:
            return TD_INTEGER;
        case SMALLINT_NN:
        case SMALLINT_N:
            return TD_SMALLINT;
        case ARRAY_1D_NN:
        case ARRAY_1D_N:
        case ARRAY_ND_NN:
        case ARRAY_ND_N:
            return TD_CHAR;
        case BIGINT_NN:
        case BIGINT_N:
            return TD_BIGINT;
        case NUMBER_NN:
        case NUMBER_N:
            return TD_NUMBER;
        case VARBYTE_NN:
        case VARBYTE_N:
            return TD_VARBYTE;
        case BYTE_NN:
        case BYTE_N:
            return TD_BYTE;
        case LONG_VARBYTE_NN:
        case LONG_VARBYTE_N:
            return TD_LONGVARCHAR;
        case DATE_NNA:
        case DATE_NA:
            return TD_CHAR;
        case DATE_NN:
        case DATE_N:
            return TD_DATE;
        case BYTEINT_NN:
        case BYTEINT_N:
            return TD_BYTEINT;
        case TIME_NN:
        case TIME_N:
            return TD_TIME;
        case TIMESTAMP_NN:
        case TIMESTAMP_N:
            return TD_TIMESTAMP;
        case TIME_NNZ:
        case TIME_NZ:
            return TD_TIME_WITHTIMEZONE;
        case TIMESTAMP_NNZ:
        case TIMESTAMP_NZ:
            return TD_TIMESTAMP_WITHTIMEZONE;
        case INTERVAL_YEAR_NN:
        case INTERVAL_YEAR_N:
            return TD_INTERVAL_YEAR;
        case INTERVAL_YEAR_TO_MONTH_NN:
        case INTERVAL_YEAR_TO_MONTH_N:
            return TD_INTERVAL_YEARTOMONTH;
        case INTERVAL_MONTH_NN:
        case INTERVAL_MONTH_N:
            return TD_INTERVAL_MONTH;
        case INTERVAL_DAY_NN:
        case INTERVAL_DAY_N:
            return TD_INTERVAL_DAY;
        case INTERVAL_DAY_TO_HOUR_NN:
        case INTERVAL_DAY_TO_HOUR_N:
            return TD_INTERVAL_DAYTOHOUR;
        case INTERVAL_DAY_TO_MINUTE_NN:
        case INTERVAL_DAY_TO_MINUTE_N:
            return TD_INTERVAL_DAYTOMINUTE;
        case INTERVAL_DAY_TO_SECOND_NN:
        case INTERVAL_DAY_TO_SECOND_N:
            return TD_INTERVAL_DAYTOSECOND;
        case INTERVAL_HOUR_NN:
        case INTERVAL_HOUR_N:
            return TD_INTERVAL_HOUR;
        case INTERVAL_HOUR_TO_MINUTE_NN:
        case INTERVAL_HOUR_TO_MINUTE_N:
            return TD_INTERVAL_HOURTOMINUTE;
        case INTERVAL_HOUR_TO_SECOND_NN:
        case INTERVAL_HOUR_TO_SECOND_N:
            return TD_INTERVAL_HOURTOSECOND;
        case INTERVAL_MINUTE_NN:
        case INTERVAL_MINUTE_N:
            return TD_INTERVAL_MINUTE;
        case INTERVAL_MINUTE_TO_SECOND_NN:
        case INTERVAL_MINUTE_TO_SECOND_N:
            return TD_INTERVAL_MINUTETOSECOND;
        case INTERVAL_SECOND_NN:
        case INTERVAL_SECOND_N:
            return TD_INTERVAL_SECOND;
        case PERIOD_DATE_NN:
        case PERIOD_DATE_N:
            return TD_PERIOD_DATE;
        case PERIOD_TIME_NN:
        case PERIOD_TIME_N:
            return TD_PERIOD_TIME;
        case PERIOD_TIME_NNZ:
        case PERIOD_TIME_NZ:
            return TD_PERIOD_TIME_TZ;
        case PERIOD_TIMESTAMP_NN:
        case PERIOD_TIMESTAMP_N:
            return TD_PERIOD_TS;
        case PERIOD_TIMESTAMP_NNZ:
        case PERIOD_TIMESTAMP_NZ:
            return TD_PERIOD_TS_TZ;
        case XML_TEXT_NN:
        case XML_TEXT_N:
        case XML_TEXT_DEFERRED_NN:
        case XML_TEXT_DEFERRED_N:
        case XML_TEXT_LOCATOR_NN:
        case XML_TEXT_LOCATOR_N:
            return TD_CHAR;
    }
    return TD_CHAR;
}

uint16_t teradata_type_from_tpt_type(uint16_t t) {
    switch (t) {
        case TD_INTEGER:
            return INTEGER_NN;
        case TD_SMALLINT:
            return SMALLINT_NN;
        case TD_FLOAT:
            return FLOAT_NN;
        case TD_DECIMAL:
            return DECIMAL_NN;
        case TD_CHAR:
            return CHAR_NN;
        case TD_BYTEINT:
            return BYTEINT_NN;
        case TD_VARCHAR:
            return VARCHAR_NN;
        case TD_LONGVARCHAR:
            return LONG_VARCHAR_NN;
        case TD_BYTE:
            return BYTE_NN;
        case TD_VARBYTE:
            return VARBYTE_NN;
        case TD_DATE:
            return DATE_NN;
        case TD_GRAPHIC:
            return GRAPHIC_NN;
        case TD_VARGRAPHIC:
            return VARGRAPHIC_NN;
        case TD_LONGVARGRAPHIC:
            return LONG_VARGRAPHIC_NN;
        case TD_DATE_ANSI:
            return DATE_NNA;
        case TD_TIME:
            return TIME_NN;
        case TD_TIME_WITHTIMEZONE:
            return TIME_NNZ;
        case TD_BIGINT:
            return BIGINT_NN;
        case TD_BLOB:
            return BLOB_NN;
        case TD_CLOB:
            return CLOB_NN;
        case TD_BLOB_AS_DEFERRED_BY_NAME:
            return BLOB_AS_DEFERRED_NAME_NN;
        case TD_CLOB_AS_DEFERRED_BY_NAME:
            return CLOB_AS_DEFERRED_NN;
        case TD_TIMESTAMP:
            return TIMESTAMP_NN;
        case TD_TIMESTAMP_WITHTIMEZONE:
            return TIMESTAMP_NNZ;
        case TD_INTERVAL_YEAR:
            return INTERVAL_YEAR_NN;
        case TD_INTERVAL_YEARTOMONTH:
            return INTERVAL_YEAR_TO_MONTH_NN;
        case TD_INTERVAL_MONTH:
            return INTERVAL_MONTH_NN;
        case TD_INTERVAL_DAY:
            return INTERVAL_DAY_NN;
        case TD_INTERVAL_DAYTOHOUR:
            return INTERVAL_DAY_TO_HOUR_NN;
        case TD_INTERVAL_DAYTOMINUTE:
            return INTERVAL_DAY_TO_MINUTE_NN;
        case TD_INTERVAL_DAYTOSECOND:
            return INTERVAL_DAY_TO_SECOND_NN;
        case TD_INTERVAL_HOUR:
            return INTERVAL_HOUR_NN;
        case TD_INTERVAL_HOURTOMINUTE:
            return INTERVAL_HOUR_TO_MINUTE_NN;
        case TD_INTERVAL_HOURTOSECOND:
            return INTERVAL_HOUR_TO_SECOND_NN;
        case TD_INTERVAL_MINUTE:
            return INTERVAL_MINUTE_NN;
        case TD_INTERVAL_MINUTETOSECOND:
            return INTERVAL_MINUTE_TO_SECOND_NN;
        case TD_INTERVAL_SECOND:
            return INTERVAL_SECOND_NN;
        case TD_PERIOD_DATE:
            return PERIOD_DATE_NN;
        case TD_PERIOD_TIME:
            return PERIOD_TIME_NN;
        case TD_PERIOD_TIME_TZ:
            return PERIOD_TIME_NNZ;
        case TD_PERIOD_TS:
            return PERIOD_TIMESTAMP_NN;
        case TD_PERIOD_TS_TZ:
            return PERIOD_TIMESTAMP_NNZ;
        case TD_NUMBER:
            return NUMBER_NN;
    }
    return CHAR_NN;
}

uint16_t teradata_type_to_giraffez_type(uint16_t t) {
    switch (t) {
        case BLOB_NN:
        case BLOB_N:
        case BLOB_AS_DEFERRED_NN:
        case BLOB_AS_DEFERRED_N:
        case BLOB_AS_LOCATOR_NN:
        case BLOB_AS_LOCATOR_N:
        case BLOB_AS_DEFERRED_NAME_NN:
        case BLOB_AS_DEFERRED_NAME_N:
        case CLOB_NN:
        case CLOB_N:
        case CLOB_AS_DEFERRED_NN:
        case CLOB_AS_DEFERRED_N:
        case CLOB_AS_LOCATOR_NN:
        case CLOB_AS_LOCATOR_N:
        case UDT_NN:
        case UDT_N:
        case DISTINCT_UDT_NN:
        case DISTINCT_UDT_N:
        case STRUCT_UDT_NN:
        case STRUCT_UDT_N:
            return GD_DEFAULT;
        case VARCHAR_NN:
        case VARCHAR_N:
            return GD_VARCHAR;
        case CHAR_NN:
        case CHAR_N:
            return GD_CHAR;
        case LONG_VARCHAR_NN:
        case LONG_VARCHAR_N:
        case VARGRAPHIC_NN:
        case VARGRAPHIC_N:
            return GD_VARCHAR;
        case GRAPHIC_NN:
        case GRAPHIC_N:
            return GD_DEFAULT;
        case LONG_VARGRAPHIC_NN:
        case LONG_VARGRAPHIC_N:
            return GD_VARCHAR;
        case FLOAT_NN:
        case FLOAT_N:
            return GD_FLOAT;
        case DECIMAL_NN:
        case DECIMAL_N:
            return GD_DECIMAL;
        case INTEGER_NN:
        case INTEGER_N:
            return GD_INTEGER;
        case SMALLINT_NN:
        case SMALLINT_N:
            return GD_SMALLINT;
        case ARRAY_1D_NN:
        case ARRAY_1D_N:
        case ARRAY_ND_NN:
        case ARRAY_ND_N:
            return GD_DEFAULT;
        case BIGINT_NN:
        case BIGINT_N:
            return GD_BIGINT;
        case NUMBER_NN:
        case NUMBER_N:
            return GD_NUMBER;
        case VARBYTE_NN:
        case VARBYTE_N:
            return GD_VARBYTE;
        case BYTE_NN:
        case BYTE_N:
            return GD_BYTE;
        case LONG_VARBYTE_NN:
        case LONG_VARBYTE_N:
            return GD_VARBYTE;
        case DATE_NNA:
        case DATE_NA:
            return GD_DEFAULT;
        case DATE_NN:
        case DATE_N:
            return GD_DATE;
        case BYTEINT_NN:
        case BYTEINT_N:
            return GD_BYTEINT;
        case TIME_NN:
        case TIME_N:
            return GD_TIME;
        case TIMESTAMP_NN:
        case TIMESTAMP_N:
            return GD_TIMESTAMP;
        case TIME_NNZ:
        case TIME_NZ:
            return GD_CHAR;
        case TIMESTAMP_NNZ:
        case TIMESTAMP_NZ:
            return GD_CHAR;
        case INTERVAL_YEAR_NN:
        case INTERVAL_YEAR_N:
        case INTERVAL_YEAR_TO_MONTH_NN:
        case INTERVAL_YEAR_TO_MONTH_N:
        case INTERVAL_MONTH_NN:
        case INTERVAL_MONTH_N:
        case INTERVAL_DAY_NN:
        case INTERVAL_DAY_N:
        case INTERVAL_DAY_TO_HOUR_NN:
        case INTERVAL_DAY_TO_HOUR_N:
        case INTERVAL_DAY_TO_MINUTE_NN:
        case INTERVAL_DAY_TO_MINUTE_N:
        case INTERVAL_DAY_TO_SECOND_NN:
        case INTERVAL_DAY_TO_SECOND_N:
        case INTERVAL_HOUR_NN:
        case INTERVAL_HOUR_N:
        case INTERVAL_HOUR_TO_MINUTE_NN:
        case INTERVAL_HOUR_TO_MINUTE_N:
        case INTERVAL_HOUR_TO_SECOND_NN:
        case INTERVAL_HOUR_TO_SECOND_N:
        case INTERVAL_MINUTE_NN:
        case INTERVAL_MINUTE_N:
        case INTERVAL_MINUTE_TO_SECOND_NN:
        case INTERVAL_MINUTE_TO_SECOND_N:
        case INTERVAL_SECOND_NN:
        case INTERVAL_SECOND_N:
        case PERIOD_DATE_NN:
        case PERIOD_DATE_N:
        case PERIOD_TIME_NN:
        case PERIOD_TIME_N:
        case PERIOD_TIME_NNZ:
        case PERIOD_TIME_NZ:
        case PERIOD_TIMESTAMP_NN:
        case PERIOD_TIMESTAMP_N:
        case PERIOD_TIMESTAMP_NNZ:
        case PERIOD_TIMESTAMP_NZ:
        case XML_TEXT_NN:
        case XML_TEXT_N:
        case XML_TEXT_DEFERRED_NN:
        case XML_TEXT_DEFERRED_N:
        case XML_TEXT_LOCATOR_NN:
        case XML_TEXT_LOCATOR_N:
            return GD_DEFAULT;
    }
    return GD_DEFAULT;
}
