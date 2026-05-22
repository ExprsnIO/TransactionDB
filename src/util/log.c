#include "tdb/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <syslog.h>
#elif defined(__linux__)
#include <syslog.h>
#endif

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

/** Return a short string label for a log level. */
static const char *level_str(tdb_log_level_t level)
{
    switch (level) {
        case TDB_LOG_DEBUG: return "DEBUG";
        case TDB_LOG_INFO:  return "INFO";
        case TDB_LOG_WARN:  return "WARN";
        case TDB_LOG_ERROR: return "ERROR";
        case TDB_LOG_FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

/** Return a short string label for an audit event type. */
static const char *audit_event_str(tdb_audit_event_t event)
{
    switch (event) {
        case TDB_AUDIT_SELECT: return "SELECT";
        case TDB_AUDIT_INSERT: return "INSERT";
        case TDB_AUDIT_UPDATE: return "UPDATE";
        case TDB_AUDIT_DELETE: return "DELETE";
        case TDB_AUDIT_DDL:    return "DDL";
        case TDB_AUDIT_AUTH:   return "AUTH";
        case TDB_AUDIT_LOCK:   return "LOCK";
    }
    return "UNKNOWN";
}

/** Map tdb_log_level_t to syslog(3) priority. */
static int level_to_syslog_prio(tdb_log_level_t level)
{
#if defined(__APPLE__) || defined(__linux__)
    switch (level) {
        case TDB_LOG_DEBUG: return LOG_DEBUG;
        case TDB_LOG_INFO:  return LOG_INFO;
        case TDB_LOG_WARN:  return LOG_WARNING;
        case TDB_LOG_ERROR: return LOG_ERR;
        case TDB_LOG_FATAL: return LOG_CRIT;
    }
    return LOG_NOTICE;
#else
    (void)level;
    return 0;
#endif
}

/**
 * Write an ISO-8601 timestamp (with milliseconds) into @p buf.
 * Example: "2024-01-15T10:30:45.123Z"
 * @p size must be >= 25.
 */
static void format_timestamp(char *buf, size_t size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm_utc;
    gmtime_r(&tv.tv_sec, &tm_utc);

    int ms = (int)(tv.tv_usec / 1000);

    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_utc.tm_year + 1900,
             tm_utc.tm_mon + 1,
             tm_utc.tm_mday,
             tm_utc.tm_hour,
             tm_utc.tm_min,
             tm_utc.tm_sec,
             ms);
}

/**
 * Write a fully-formatted log line to the destination configured in @p logger.
 * @p msg is the already-formatted user message (no trailing newline).
 */
static void emit(tdb_logger_t *logger, tdb_log_level_t level, const char *msg)
{
    char ts[32];
    format_timestamp(ts, sizeof(ts));

    char line[2048];
    int len = snprintf(line, sizeof(line), "%s [%s] %s\n",
                       ts, level_str(level), msg);
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof(line)) {
        len = (int)sizeof(line) - 1;
    }

    switch (logger->dest) {
        case TDB_LOG_DEST_STDOUT:
            (void)fwrite(line, 1, (size_t)len, stdout);
            fflush(stdout);
            break;

        case TDB_LOG_DEST_STDERR:
            (void)fwrite(line, 1, (size_t)len, stderr);
            fflush(stderr);
            break;

        case TDB_LOG_DEST_FILE:
            if (logger->fd >= 0) {
                (void)write(logger->fd, line, (size_t)len);
            }
            break;

        case TDB_LOG_DEST_SYSLOG:
#if defined(__APPLE__) || defined(__linux__)
            syslog(level_to_syslog_prio(level), "%s", msg);
#endif
            break;
    }
}

/* -----------------------------------------------------------------------
 * Logger public API
 * --------------------------------------------------------------------- */

void tdb_log_init(tdb_logger_t *logger)
{
    if (!logger) {
        return;
    }
    logger->level = TDB_LOG_INFO;
    logger->dest  = TDB_LOG_DEST_STDERR;
    logger->fd    = -1;
    memset(logger->format, 0, sizeof(logger->format));
    strncpy(logger->format, "%T [%L] %M", sizeof(logger->format) - 1);
}

void tdb_log_set_level(tdb_logger_t *logger, tdb_log_level_t level)
{
    if (!logger) {
        return;
    }
    logger->level = level;
}

void tdb_log_set_destination(tdb_logger_t *logger, tdb_log_dest_t dest,
                             const char *path)
{
    if (!logger) {
        return;
    }

    /* Close previous file descriptor if we were using a file. */
    if (logger->dest == TDB_LOG_DEST_FILE && logger->fd >= 0) {
        close(logger->fd);
        logger->fd = -1;
    }

#if defined(__APPLE__) || defined(__linux__)
    /* Close previous syslog connection if switching away from syslog. */
    if (logger->dest == TDB_LOG_DEST_SYSLOG && dest != TDB_LOG_DEST_SYSLOG) {
        closelog();
    }
#endif

    logger->dest = dest;

    if (dest == TDB_LOG_DEST_FILE && path) {
        logger->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        /* If open fails, fd stays -1 and emit() silently drops messages. */
    }

#if defined(__APPLE__) || defined(__linux__)
    if (dest == TDB_LOG_DEST_SYSLOG) {
        openlog("tdb", LOG_PID | LOG_NDELAY, LOG_USER);
    }
#endif
}

void tdb_log_destroy(tdb_logger_t *logger)
{
    if (!logger) {
        return;
    }

    if (logger->dest == TDB_LOG_DEST_FILE && logger->fd >= 0) {
        close(logger->fd);
        logger->fd = -1;
    }

#if defined(__APPLE__) || defined(__linux__)
    if (logger->dest == TDB_LOG_DEST_SYSLOG) {
        closelog();
    }
#endif

    logger->level = TDB_LOG_INFO;
    logger->dest  = TDB_LOG_DEST_STDERR;
}

void tdb_log(tdb_logger_t *logger, tdb_log_level_t level,
             const char *fmt, ...)
{
    if (!logger || !fmt) {
        return;
    }

    /* Filter by minimum level. */
    if (level < logger->level) {
        return;
    }

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    emit(logger, level, msg);
}

/* -----------------------------------------------------------------------
 * Audit public API
 * --------------------------------------------------------------------- */

void tdb_audit_init(tdb_audit_config_t *audit, tdb_logger_t *logger)
{
    if (!audit) {
        return;
    }
    memset(audit, 0, sizeof(*audit));
    audit->logger = logger;
    audit->dest   = logger ? logger->dest : TDB_LOG_DEST_STDERR;
}

void tdb_audit_enable_event(tdb_audit_config_t *audit,
                            tdb_audit_event_t event)
{
    if (!audit) {
        return;
    }
    audit->enabled_events |= (uint32_t)event;
}

void tdb_audit_disable_event(tdb_audit_config_t *audit,
                             tdb_audit_event_t event)
{
    if (!audit) {
        return;
    }
    audit->enabled_events &= ~(uint32_t)event;
}

int tdb_audit_enable_table(tdb_audit_config_t *audit, const char *table_name)
{
    if (!audit || !table_name) {
        return -1;
    }
    if (audit->table_count >= TDB_AUDIT_MAX_TABLES) {
        return -1;
    }

    /* Avoid duplicates. */
    for (uint32_t i = 0; i < audit->table_count; i++) {
        if (strncmp(audit->tables[i], table_name,
                    TDB_AUDIT_TABLE_NAME_MAX) == 0) {
            return 0;   /* already present */
        }
    }

    strncpy(audit->tables[audit->table_count], table_name,
            TDB_AUDIT_TABLE_NAME_MAX - 1);
    audit->tables[audit->table_count][TDB_AUDIT_TABLE_NAME_MAX - 1] = '\0';
    audit->table_count++;
    return 0;
}

/**
 * Check whether @p table should be audited according to @p audit.
 *
 * If no tables have been explicitly listed every table is audited.
 * If at least one table is listed, only those tables are audited.
 */
static bool table_is_audited(const tdb_audit_config_t *audit,
                             const char *table)
{
    if (audit->table_count == 0) {
        return true;            /* no filter -- audit everything */
    }
    if (!table) {
        return true;            /* no table context -- always log */
    }
    for (uint32_t i = 0; i < audit->table_count; i++) {
        if (strncmp(audit->tables[i], table,
                    TDB_AUDIT_TABLE_NAME_MAX) == 0) {
            return true;
        }
    }
    return false;
}

void tdb_audit_log(tdb_audit_config_t *audit, tdb_audit_event_t event,
                   const char *table, const char *detail, ...)
{
    if (!audit || !audit->logger) {
        return;
    }

    /* Check whether this event type is enabled. */
    if (!(audit->enabled_events & (uint32_t)event)) {
        return;
    }

    /* Check per-table filtering. */
    if (!table_is_audited(audit, table)) {
        return;
    }

    /* Format the caller-supplied detail string. */
    char detail_buf[512];
    detail_buf[0] = '\0';
    if (detail) {
        va_list ap;
        va_start(ap, detail);
        vsnprintf(detail_buf, sizeof(detail_buf), detail, ap);
        va_end(ap);
    }

    /* Build the complete audit message. */
    char audit_msg[1024];
    snprintf(audit_msg, sizeof(audit_msg), "AUDIT event=%s table=%s %s",
             audit_event_str(event),
             table ? table : "(none)",
             detail_buf);

    emit(audit->logger, TDB_LOG_INFO, audit_msg);
}
