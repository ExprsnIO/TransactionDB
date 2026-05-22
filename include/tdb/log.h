#ifndef TDB_LOG_H
#define TDB_LOG_H

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Log levels
 * ------------------------------------------------------------------------- */
typedef enum {
    TDB_LOG_DEBUG = 0,
    TDB_LOG_INFO,
    TDB_LOG_WARN,
    TDB_LOG_ERROR,
    TDB_LOG_FATAL
} tdb_log_level_t;

/* ---------------------------------------------------------------------------
 * Log destinations
 * ------------------------------------------------------------------------- */
typedef enum {
    TDB_LOG_DEST_STDOUT = 0,
    TDB_LOG_DEST_STDERR,
    TDB_LOG_DEST_FILE,
    TDB_LOG_DEST_SYSLOG
} tdb_log_dest_t;

/* ---------------------------------------------------------------------------
 * Audit event types (bitmask)
 * ------------------------------------------------------------------------- */
typedef enum {
    TDB_AUDIT_SELECT = (1 << 0),
    TDB_AUDIT_INSERT = (1 << 1),
    TDB_AUDIT_UPDATE = (1 << 2),
    TDB_AUDIT_DELETE = (1 << 3),
    TDB_AUDIT_DDL    = (1 << 4),
    TDB_AUDIT_AUTH   = (1 << 5),
    TDB_AUDIT_LOCK   = (1 << 6)
} tdb_audit_event_t;

/* ---------------------------------------------------------------------------
 * Maximum number of individually-audited tables
 * ------------------------------------------------------------------------- */
#define TDB_AUDIT_MAX_TABLES 64

/* Maximum length of an audited table name (including NUL) */
#define TDB_AUDIT_TABLE_NAME_MAX 128

/* ---------------------------------------------------------------------------
 * Logger structure
 *
 * All state is instance-local; there is no global state.
 * ------------------------------------------------------------------------- */
typedef struct {
    tdb_log_level_t level;           /* minimum level to emit                */
    tdb_log_dest_t  dest;            /* where to write                       */
    int             fd;              /* file descriptor when dest == FILE    */
    char            format[256];     /* format pattern (reserved for future) */
} tdb_logger_t;

/* ---------------------------------------------------------------------------
 * Audit configuration
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t        enabled_events;  /* bitmask of tdb_audit_event_t         */
    tdb_log_dest_t  dest;            /* where to write audit records         */
    tdb_logger_t   *logger;          /* underlying logger for output         */

    /* Per-table audit overrides.  When table_count > 0, only the listed
       tables are audited.  When table_count == 0, all tables are audited
       (subject to enabled_events). */
    char            tables[TDB_AUDIT_MAX_TABLES][TDB_AUDIT_TABLE_NAME_MAX];
    uint32_t        table_count;
} tdb_audit_config_t;

/* ---------------------------------------------------------------------------
 * Logger API
 * ------------------------------------------------------------------------- */

/** Initialise a logger with defaults (level = INFO, dest = STDERR). */
void tdb_log_init(tdb_logger_t *logger);

/** Change the minimum log level. */
void tdb_log_set_level(tdb_logger_t *logger, tdb_log_level_t level);

/** Change the log destination.  @p path is used when dest == TDB_LOG_DEST_FILE. */
void tdb_log_set_destination(tdb_logger_t *logger, tdb_log_dest_t dest,
                             const char *path);

/** Release resources held by the logger (e.g. close file descriptor). */
void tdb_log_destroy(tdb_logger_t *logger);

/** Emit a log message (printf-style). */
void tdb_log(tdb_logger_t *logger, tdb_log_level_t level,
             const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
;

/* Convenience macros */
#define tdb_log_debug(logger, ...) \
    tdb_log((logger), TDB_LOG_DEBUG, __VA_ARGS__)

#define tdb_log_info(logger, ...) \
    tdb_log((logger), TDB_LOG_INFO, __VA_ARGS__)

#define tdb_log_warn(logger, ...) \
    tdb_log((logger), TDB_LOG_WARN, __VA_ARGS__)

#define tdb_log_error(logger, ...) \
    tdb_log((logger), TDB_LOG_ERROR, __VA_ARGS__)

#define tdb_log_fatal(logger, ...) \
    tdb_log((logger), TDB_LOG_FATAL, __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Audit API
 * ------------------------------------------------------------------------- */

/** Initialise audit configuration and bind it to an existing logger. */
void tdb_audit_init(tdb_audit_config_t *audit, tdb_logger_t *logger);

/** Enable a specific audit event type. */
void tdb_audit_enable_event(tdb_audit_config_t *audit,
                            tdb_audit_event_t event);

/** Disable a specific audit event type. */
void tdb_audit_disable_event(tdb_audit_config_t *audit,
                             tdb_audit_event_t event);

/** Add a table to the per-table audit list.  Returns -1 if the list is full. */
int tdb_audit_enable_table(tdb_audit_config_t *audit, const char *table_name);

/** Log an audit event (printf-style detail). */
void tdb_audit_log(tdb_audit_config_t *audit, tdb_audit_event_t event,
                   const char *table, const char *detail, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
;

#ifdef __cplusplus
}
#endif

#endif /* TDB_LOG_H */
