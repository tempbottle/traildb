
#ifndef __TDB_ERROR_H__
#define __TDB_ERROR_H__

/* generic */

#define TDB_ERR_NOMEM -2
#define TDB_ERR_PATH_TOO_LONG -3
#define TDB_ERR_UNKNOWN_FIELD -4
#define TDB_ERR_UNKNOWN_UUID -5
#define TDB_ERR_INVALID_TRAIL_ID -6
#define TDB_ERR_HANDLE_IS_NULL -7
#define TDB_ERR_HANDLE_ALREADY_OPENED -8

/* io */

#define TDB_ERR_IO_OPEN -65
#define TDB_ERR_IO_CLOSE -66
#define TDB_ERR_IO_WRITE -67
#define TDB_ERR_IO_READ -68
#define TDB_ERR_IO_TRUNCATE -69

/* tdb_open */

#define TDB_ERR_INVALID_INFO_FILE -129
#define TDB_ERR_INVALID_VERSION_FILE -130
#define TDB_ERR_INCOMPATIBLE_VERSION -131
#define TDB_ERR_INVALID_FIELDS_FILE -132
#define TDB_ERR_INVALID_UUIDS_FILE -133
#define TDB_ERR_INVALID_CODEBOOK_FILE -134
#define TDB_ERR_INVALID_TRAILS_FILE -135
#define TDB_ERR_INVALID_LEXICON_FILE -136

/* tdb_cons */

#define TDB_ERR_TOO_MANY_FIELDS -257
#define TDB_ERR_DUPLICATE_FIELDS -258
#define TDB_ERR_INVALID_FIELDNAME -259
#define TDB_ERR_TOO_MANY_TRAILS -260
#define TDB_ERR_VALUE_TOO_LONG -261
#define TDB_ERR_APPEND_FIELDS_MISMATCH -262
#define TDB_ERR_LEXICON_TOO_LARGE -263
#define TDB_ERR_TIMESTAMP_TOO_LARGE -264
#define TDB_ERR_TRAIL_TOO_LONG -265

#endif /* __TDB_ERROR_H__ */
