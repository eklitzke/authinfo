/* -*- mode: c; c-basic-offset: 4; tab-width: 4; ; indent-tabs-mode: nil; -*- */
/**
 * @file   authinfo.c
 * @author Aliaksey Artamonau <aliaksiej.artamonau@gmail.com>
 *
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include "authinfo.h"
#include "authinfo_internal.h"

#define DOT "."
#define AUTHINFO "authinfo"
#define DOT_AUTHINFO (DOT AUTHINFO)
#define NETRC "netrc"
#define DOT_NETRC (DOT NETRC)

#define TOKEN_SIZE_MAX 128

/* internal macros */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
/* internal macros end */

/* internal functions prototypes */
static enum authinfo_result_t authinfo_errno2result(int errnum);

static char *authinfo_path_join(const char *dir, const char *name);

static enum authinfo_result_t authinfo_path_probe(const char *path);

static enum authinfo_result_t
authinfo_find_file_in_dir(const char *dir, const char *name, char **pathp);

static void authinfo_skip_spaces(const char **str, unsigned int *column);

static bool authinfo_eol(const char *str);

static bool authinfo_eof(const char *str);

static void authinfo_skip_line(const char **str,
                               unsigned int *line, unsigned int *column);

static bool authinfo_skip_comment(const char **str,
                                  unsigned int *line, unsigned int *column);

static bool authinfo_skip_macdef(const char **str,
                                 unsigned int *line, unsigned int *column);

static bool authinfo_report_entry(authinfo_parse_entry_cb_t entry_callback,
                                  authinfo_parse_error_cb_t error_callback,
                                  void *arg,
                                  unsigned int line,
                                  const struct authinfo_parse_entry_t *entry);

static bool
authinfo_report_error(authinfo_parse_error_cb_t error_callback, void *arg,
                      enum authinfo_parse_error_type_t type,
                      unsigned int line, unsigned int column);

static bool
authinfo_next_token(const char **str, unsigned int *column, char *token);
/* internal functions prototypes end */

static const char *authinfo_result2str[] = {
    [AUTHINFO_OK] = "Success",
    [AUTHINFO_EACCESS] = "Permission denied",
    [AUTHINFO_ENOENT] = "File or directory not found",
    [AUTHINFO_ENOMEM] = "Could not allocate memory",
    [AUTHINFO_TOOBIG] = "Authinfo file is too big",
    [AUTHINFO_EUNKNOWN] = "Unknown error happened",
};

const char *
authinfo_strerror(enum authinfo_result_t status)
{
    if (status >= AUTHINFO_RESULT_MAX) {
        return "Got unexpected status code";
    }

    return authinfo_result2str[status];
}

enum authinfo_result_t
authinfo_find_file(char **path)
{
    char *home;
    enum authinfo_result_t ret;

    home = getenv("HOME");
    if (home) {
        ret = authinfo_find_file_in_dir(home, DOT_AUTHINFO, path);
        if (ret != AUTHINFO_ENOENT) {
            /* we either successfully found the file or got some error */
            return ret;
        }

        ret = authinfo_find_file_in_dir(home, DOT_NETRC, path);
        if (ret != AUTHINFO_ENOENT) {
            return ret;
        }
    }

    ret = authinfo_find_file_in_dir(SYSCONF_DIR, AUTHINFO, path);
    if (ret != AUTHINFO_ENOENT) {
        return ret;
    }

    return authinfo_find_file_in_dir(SYSCONF_DIR, NETRC, path);
}

enum authinfo_result_t
authinfo_read_file(const char *path, char *buffer, size_t size)
{
    int fd;

    while (true) {
        fd = open(path, O_RDONLY);
        if (fd != -1) {
            break;
        }

        if (errno == EINTR) {
            continue;
        } else {
            TRACE("Could not open authinfo file: %s\n", strerror(errno));
            return authinfo_errno2result(errno);
        }
    }

    while (true) {
        ssize_t nread;

        if (size == 0) {
            return AUTHINFO_TOOBIG;
        }

        nread = read(fd, buffer, MIN(size, 0xffff));
        if (nread == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                TRACE("Could not read authinfo file: %s\n", strerror(errno));
                return authinfo_errno2result(errno);
            }
        } else if (nread == 0) {
            assert(size != 0);
            *buffer = '\0';
            return AUTHINFO_OK;
        }

        assert(size >= nread);

        size -= nread;
        buffer += nread;
    }
}

enum parse_state_t {
    LINE_START,
    WAITING_NEXT_PAIR,
    WAITING_HOST,
    WAITING_PROTOCOL,
    WAITING_USER,
    WAITING_PASSWORD,
    WAITING_FORCE,
    LINE_END,
    PARSE_STATE_MAX
};

#ifdef DEBUG
const char *parse_state2str(enum parse_state_t state)
{
    const char *strs[] = {
        [LINE_START] = "LINE_START",
        [WAITING_NEXT_PAIR] = "WAITING_NEXT_PAIR",
        [WAITING_HOST] = "WAITING_HOST",
        [WAITING_PROTOCOL] = "WAITING_PROTOCOL",
        [WAITING_USER] = "WAITING_USER",
        [WAITING_PASSWORD] = "WAITING_PASSWORD",
        [WAITING_FORCE] = "WAITING_FORCE",
        [LINE_END] = "LINE_END"
    };

    if (state >= PARSE_STATE_MAX) {
        return "UNKNOWN";
    } else {
        return strs[state];
    }
}
#endif

void
authinfo_parse(const char *data, void *arg,
               authinfo_parse_entry_cb_t entry_callback,
               authinfo_parse_error_cb_t error_callback)
{
    unsigned int line = 1;
    unsigned int column = 0;

    char host[TOKEN_SIZE_MAX];
    char protocol[TOKEN_SIZE_MAX];
    char user[TOKEN_SIZE_MAX];
    char password[TOKEN_SIZE_MAX];

    struct authinfo_parse_entry_t entry;

    bool stop = false;

    enum parse_state_t state = LINE_START;

    while (!stop) {
        char token[TOKEN_SIZE_MAX];

        unsigned long token_column = column;
        authinfo_skip_spaces(&data, &column);

        TRACE("\n");
        TRACE("State: %s\n", parse_state2str(state));
        TRACE("Position: %u:%u\n", line, column);

        if (authinfo_eol(data)) {
            TRACE("Encountered EOL at %u:%u\n", line, column);

            switch (state) {
            case LINE_START:
                /* is this really a EOF? */
                if (authinfo_eof(data)) {
                    TRACE("Encountered EOF at %u:%u\n", line, column);
                    stop = true;
                } else {
                    TRACE("Skipping empty line %u\n", line);
                    /* we haven't read anything; just go to the next line */
                    authinfo_skip_line(&data, &line, &column);
                }
                continue;
            case WAITING_NEXT_PAIR:
                /* report the entry */
                state = LINE_END;
                break;
            default:
                state = LINE_END;
                /* we were waiting for some value; report an error */
                stop = authinfo_report_error(error_callback, arg,
                                             AUTHINFO_PET_MISSING_VALUE,
                                             line, token_column);

            }
        }

        TRACE("Updated state: %s\n", parse_state2str(state));

        switch (state) {
        case LINE_START:
            entry.host = NULL;
            entry.protocol = NULL;
            entry.user = NULL;
            entry.password = NULL;
            entry.force = false;

            if (!authinfo_skip_comment(&data, &line, &column)) {
                if (!authinfo_skip_macdef(&data, &line, &column)) {
                    state = WAITING_NEXT_PAIR;
                }
            }

            break;
        case LINE_END:
            stop = authinfo_report_entry(entry_callback, error_callback, arg,
                                         line, &entry);
            authinfo_skip_line(&data, &line, &column);
            state = LINE_START;
            break;
        case WAITING_NEXT_PAIR:
            if (authinfo_next_token(&data, &column, token)) {
                bool report_duplicate = false;

                TRACE("Read token \"%s\"\n", token);

                if (strcmp(token, "default") == 0) {
                    /* empty will indicate the default match entry */
                    host[0] = '\0';
                    report_duplicate = (entry.host != NULL);
                    entry.host = host;
                } else if (strcmp(token, "machine") == 0 ||
                           strcmp(token, "host") == 0) {
                    state = WAITING_HOST;
                    report_duplicate = (entry.host != NULL);
                } else if (strcmp(token, "login") == 0 ||
                           strcmp(token, "user") == 0 ||
                           strcmp(token, "account") == 0) {
                    state = WAITING_USER;
                    report_duplicate = (entry.user != NULL);
                } else if (strcmp(token, "password") == 0) {
                    state = WAITING_PASSWORD;
                    report_duplicate = (entry.password != NULL);
                } else if (strcmp(token, "force") == 0) {
                    state = WAITING_FORCE;
                    report_duplicate = entry.force;
                } else if (strcmp(token, "port") == 0 ||
                           strcmp(token, "protocol") == 0) {
                    state = WAITING_PROTOCOL;
                    report_duplicate = (entry.protocol != NULL);
                } else {
                    stop = authinfo_report_error(error_callback, arg,
                                                 AUTHINFO_PET_BAD_KEYWORD,
                                                 line, token_column);
                }

                if (report_duplicate) {
                    stop = authinfo_report_error(error_callback, arg,
                                                 AUTHINFO_PET_DUPLICATED_KEYWORD,
                                                 line, token_column);

                }

                break;
            } else {
                authinfo_report_error(error_callback, arg,
                                      AUTHINFO_PET_BAD_KEYWORD,
                                      line, token_column);
                break;
            }
            break;
        case WAITING_HOST:
        case WAITING_PROTOCOL:
        case WAITING_USER:
        case WAITING_PASSWORD:
        case WAITING_FORCE:
            if (authinfo_next_token(&data, &column, token)) {
                TRACE("Read token \"%s\"\n", token);

                switch (state) {
                case WAITING_FORCE:
                    if (strcmp(token, "yes") == 0) {
                        entry.force = true;
                    } else {
                        authinfo_report_error(error_callback, arg,
                                              AUTHINFO_PET_BAD_VALUE,
                                              line, token_column);
                    }

                    break;

#define ASSIGN(name) if (entry.name == NULL) {   \
                        strcpy(name, token);     \
                        entry.name = name;       \
                     }

                case WAITING_HOST:
                    ASSIGN(host);
                    break;
                case WAITING_PROTOCOL:
                    ASSIGN(protocol);
                    break;
                case WAITING_USER:
                    ASSIGN(user);
                    break;
                case WAITING_PASSWORD:
                    ASSIGN(password);
                    break;
                default:
                    /* should not happen */
                    assert(false);

                }
#undef ASSIGN

                state = WAITING_NEXT_PAIR;

                break;
            } else {
                stop = authinfo_report_error(error_callback, arg,
                                             AUTHINFO_PET_VALUE_TOO_LONG,
                                             line, token_column);
                state = WAITING_NEXT_PAIR;
            }

            break;
        default:
            /* should not happen */
            assert(false);
        }
    }
}


static const char *authinfo_parse_error_type2str[] = {
    [AUTHINFO_PET_MISSING_HOST] = "Host not specified",
    [AUTHINFO_PET_MISSING_VALUE] = "Expected a value",
    [AUTHINFO_PET_VALUE_TOO_LONG] = "Value is too long",
    [AUTHINFO_PET_BAD_VALUE] = "Invalid value",
    [AUTHINFO_PET_BAD_KEYWORD] = "Unknown keyword used",
    [AUTHINFO_PET_DUPLICATED_KEYWORD] = "Duplicate or synonymous keyword",
};

const char *
authinfo_parse_strerror(enum authinfo_parse_error_type_t error)
{
    if (error >= AUTHINFO_PET_MAX) {
        return "Unknown";
    } else {
        return authinfo_parse_error_type2str[error];
    }
}

/* internal */
static enum authinfo_result_t
authinfo_errno2result(int errnum)
{
    enum authinfo_result_t ret;

    switch (errnum) {
    case EACCES:
        ret = AUTHINFO_EACCESS;
        break;
    case ENOENT:
    case ENOTDIR:
    case ELOOP:
        ret = AUTHINFO_ENOENT;
        break;
    case ENOMEM:
        ret = AUTHINFO_ENOMEM;
        break;
    default:
        ret = AUTHINFO_EUNKNOWN;
    }

    return ret;
}

static char *
authinfo_path_join(const char *dir, const char *name)
{
    char *path = NULL;
    size_t length = strlen(dir) + strlen(name) + 2;

    path = malloc(length);
    if (!path) {
        TRACE("could not allocate %zu bytes\n", length);
        return NULL;
    }

    snprintf(path, length, "%s/%s", dir, name);
    return path;
}

static enum authinfo_result_t
authinfo_find_file_in_dir(const char *dir, const char *name, char **pathp)
{
    char *path;
    enum authinfo_result_t ret;

    path = authinfo_path_join(dir, name);
    if (!path) {
        return AUTHINFO_ENOMEM;
    }
    *pathp = path;

    ret = authinfo_path_probe(path);
    TRACE("Probed %s: %s\n", path, authinfo_strerror(ret));

    if (ret != AUTHINFO_OK) {
        free(path);
    }

    return ret;
}

static enum authinfo_result_t
authinfo_path_probe(const char *path)
{
    enum authinfo_result_t ret = AUTHINFO_OK;

    if (access(path, R_OK) != 0) {
        ret = authinfo_errno2result(errno);
    }

    return ret;
}

static void
authinfo_skip_spaces(const char **str, unsigned int *column)
{
    size_t span;

    span = strspn(*str, " \t");
    *str += span;
    *column += span;
}

static bool
authinfo_eol(const char *str)
{
    return *str == '\n' ||
        *str == '\0';
}

static bool
authinfo_eof(const char *str)
{
    return *str == '\0';
}

static void
authinfo_skip_line(const char **str, unsigned int *line, unsigned int *column)
{
    size_t cspan;

    cspan = strcspn(*str, "\n");
    *str += cspan + 1;
    *column = 0;
    *line += 1;
}

static bool
authinfo_skip_comment(const char **str,
                      unsigned int *line, unsigned int *column)
{
    const char *p = *str;

    if (*p == '#') {
        TRACE("Skipping comment at line %u\n", *line);
        authinfo_skip_line(str, line, column);
        return true;
    }

    return false;
}

static bool
authinfo_skip_macdef(const char **str,
                     unsigned int *line, unsigned int *column)
{
    char token[TOKEN_SIZE_MAX];

    const char *token_end = *str;
    unsigned int token_end_column = *column;

    if (authinfo_next_token(&token_end, &token_end_column, token) &&
        strcmp(token, "macdef") == 0) {
        unsigned int start_line = *line;

        *column = token_end_column;
        *str = token_end;

        do {
            authinfo_skip_line(str, line, column);
        } while (**str != '\n');

        TRACE("Skipped macdef on lines %u-%u\n", start_line, *line);

        return true;
    }

    return false;
}

static bool
authinfo_report_entry(authinfo_parse_entry_cb_t entry_callback,
                      authinfo_parse_error_cb_t error_callback,
                      void *arg,
                      unsigned int line,
                      const struct authinfo_parse_entry_t *entry)
{
    bool stop;

    if (!entry->host) {
        /* missing host is an error */
        return authinfo_report_error(error_callback, arg,
                                     AUTHINFO_PET_MISSING_HOST, line, 0);
    }

    stop = (*entry_callback)(entry, arg);
    TRACE("Reported an entry: host -> %s, protocol -> %s, "
          "user -> %s, password -> %s, force -> %d => %s\n",
          entry->host, entry->protocol, entry->user, entry->password,
          (int) entry->force,
          stop ? "stopping" : "continuing");

    return stop;
}

static bool
authinfo_report_error(authinfo_parse_error_cb_t error_callback, void *arg,
                      enum authinfo_parse_error_type_t type,
                      unsigned int line, unsigned int column)
{
    bool stop = (*error_callback)(type, line, column, arg);
    TRACE("Reported an error: %s (%u:%u) => %s\n",
          authinfo_parse_strerror(type),
          line, column, stop ? "stopping" : "continuing");

    return stop;
}

static bool
authinfo_next_token(const char **str, unsigned int *column, char *token)
{
    size_t cspan;
    bool ret = true;

    /* TODO: handle quoted tokens */
    cspan = strcspn(*str, " \t\n");

    if (cspan >= TOKEN_SIZE_MAX) {
        ret = false;
    } else {
        memcpy(token, *str, cspan);
        token[cspan] = '\0';
    }

    *column += cspan;
    *str += cspan;

    return ret;
}