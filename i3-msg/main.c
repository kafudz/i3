/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3-msg/main.c: Utility which sends messages to a running i3-instance using
 * IPC via UNIX domain sockets.
 *
 * This (in combination with libi3/ipc_send_message.c and
 * libi3/ipc_recv_message.c) serves as an example for how to send your own
 * messages to i3.
 *
 * Additionally, it’s even useful sometimes :-).
 *
 */
#include "libi3.h"

#include <err.h>
#include <getopt.h>
#include <i3/ipc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <yajl/yajl_parse.h>

/*
 * Having verboselog() and errorlog() is necessary when using libi3.
 *
 */
void verboselog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

void errorlog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

/*******************************************************************************
 * Message types
 *******************************************************************************/

typedef struct message_type_t {
    const char *name;
    uint32_t type;
    /* Aliases are accepted on the command line but not advertised. */
    bool alias;
} message_type_t;

static const message_type_t message_types[] = {
    {"command", I3_IPC_MESSAGE_TYPE_RUN_COMMAND, true},
    {"run_command", I3_IPC_MESSAGE_TYPE_RUN_COMMAND, false},
    {"get_workspaces", I3_IPC_MESSAGE_TYPE_GET_WORKSPACES, false},
    {"get_outputs", I3_IPC_MESSAGE_TYPE_GET_OUTPUTS, false},
    {"get_tree", I3_IPC_MESSAGE_TYPE_GET_TREE, false},
    {"get_marks", I3_IPC_MESSAGE_TYPE_GET_MARKS, false},
    {"get_bar_config", I3_IPC_MESSAGE_TYPE_GET_BAR_CONFIG, false},
    {"get_binding_modes", I3_IPC_MESSAGE_TYPE_GET_BINDING_MODES, false},
    {"get_binding_state", I3_IPC_MESSAGE_TYPE_GET_BINDING_STATE, false},
    {"get_version", I3_IPC_MESSAGE_TYPE_GET_VERSION, false},
    {"get_config", I3_IPC_MESSAGE_TYPE_GET_CONFIG, false},
    {"send_tick", I3_IPC_MESSAGE_TYPE_SEND_TICK, false},
    {"subscribe", I3_IPC_MESSAGE_TYPE_SUBSCRIBE, false},
};

/*
 * Looks up the message type with the given (case-insensitive) name. Returns
 * false if there is no such message type.
 *
 */
static bool lookup_message_type(const char *name, uint32_t *type) {
    for (size_t i = 0; i < sizeof(message_types) / sizeof(message_types[0]); i++) {
        if (strcasecmp(name, message_types[i].name) == 0) {
            *type = message_types[i].type;
            return true;
        }
    }
    return false;
}

/*******************************************************************************
 * Helpers
 *******************************************************************************/

/*
 * Writes the given reply followed by a newline to the given stream. Replies
 * are not NUL-terminated and their length does not necessarily fit an int, so
 * printf("%.*s") is not an option.
 *
 */
static void print_reply(FILE *stream, const uint8_t *reply, uint32_t reply_length) {
    fwrite(reply, 1, reply_length, stream);
    fputc('\n', stream);
}

/*
 * Receives a message. Reports failures and exits, so that callers only ever see
 * valid replies. Exit status 1 as documented for "unable to connect to
 * ipc-socket".
 *
 */
static void recv_message_or_die(int sockfd, uint32_t *reply_type, uint32_t *reply_length, uint8_t **reply) {
    const int ret = ipc_recv_message(sockfd, reply_type, reply_length, reply);
    switch (ret) {
        case 0:
            return;
        case -1:
            err(EXIT_FAILURE, "IPC: read()");
        case -2:
            errx(EXIT_FAILURE, "IPC: connection closed by i3");
        default:
            /* The protocol violation was already logged by libi3. */
            exit(EXIT_FAILURE);
    }
}

/*
 * Runs the given data through the JSON parser using the given callbacks.
 * Exits with an error message when the data is not (completely) valid JSON.
 *
 */
static void parse_json_or_die(const yajl_callbacks *callbacks, const uint8_t *data, uint32_t length) {
    yajl_handle handle = yajl_alloc(callbacks, NULL, NULL);
    if (handle == NULL) {
        errx(EXIT_FAILURE, "IPC: Could not allocate JSON parser.");
    }

    yajl_status state = yajl_parse(handle, data, length);
    if (state == yajl_status_ok) {
        /* Without this, a truncated reply would be accepted silently. */
        state = yajl_complete_parse(handle);
    }

    if (state != yajl_status_ok) {
        unsigned char *msg = yajl_get_error(handle, 0, data, length);
        warnx("IPC: Could not parse JSON reply: %s", (const char *)msg);
        yajl_free_error(handle, msg);
        yajl_free(handle);
        exit(EXIT_FAILURE);
    }

    yajl_free(handle);
}

static bool key_is(const char *key, const char *expected) {
    return key != NULL && strcmp(key, expected) == 0;
}

/*
 * Remembers the key of the JSON member that is currently being parsed.
 *
 */
static void remember_key(char **dest, const unsigned char *key, size_t key_length) {
    free(*dest);
    *dest = sstrndup((const char *)key, key_length);
}

/*******************************************************************************
 * Command (and subscribe) reply callbacks
 *******************************************************************************/

static char *last_key = NULL;

typedef struct reply_t {
    bool success;
    char *error;
    char *input;
    char *errorposition;
} reply_t;

static int exit_code = 0;
static reply_t last_reply;

/*
 * Resets the state collected for the current reply map. The reply to a command
 * is an array with one map per command, so nothing may leak from one map into
 * the next one (e.g. the input/errorposition of an earlier parse error).
 *
 */
static void reply_reset(void) {
    free(last_reply.error);
    free(last_reply.input);
    free(last_reply.errorposition);
    last_reply = (reply_t){0};
}

static void reply_set_string(char **dest, char *str) {
    free(*dest);
    *dest = str;
}

static int reply_boolean_cb(void *params, int val) {
    if (key_is(last_key, "success")) {
        last_reply.success = val;
    }
    return 1;
}

static int reply_string_cb(void *params, const unsigned char *val, size_t len) {
    char *str = sstrndup((const char *)val, len);

    if (key_is(last_key, "error")) {
        reply_set_string(&last_reply.error, str);
    } else if (key_is(last_key, "input")) {
        reply_set_string(&last_reply.input, str);
    } else if (key_is(last_key, "errorposition")) {
        reply_set_string(&last_reply.errorposition, str);
    } else {
        free(str);
    }
    return 1;
}

static int reply_start_map_cb(void *params) {
    reply_reset();
    return 1;
}

static int reply_end_map_cb(void *params) {
    if (!last_reply.success) {
        if (last_reply.input && last_reply.errorposition) {
            fprintf(stderr, "ERROR: Your command: %s\n", last_reply.input);
            fprintf(stderr, "ERROR:               %s\n", last_reply.errorposition);
        }
        /* Not every failure carries an error message, e.g. i3 only replies
         * {"success":false} when a subscription request cannot be parsed. */
        fprintf(stderr, "ERROR: %s\n", last_reply.error ? last_reply.error : "i3 reported a failure without an error message");
        exit_code = 2;
    }
    reply_reset();
    return 1;
}

static int reply_map_key_cb(void *params, const unsigned char *keyVal, size_t keyLen) {
    remember_key(&last_key, keyVal, keyLen);
    return 1;
}

static yajl_callbacks reply_callbacks = {
    .yajl_boolean = reply_boolean_cb,
    .yajl_string = reply_string_cb,
    .yajl_start_map = reply_start_map_cb,
    .yajl_map_key = reply_map_key_cb,
    .yajl_end_map = reply_end_map_cb,
};

/*******************************************************************************
 * Config reply callbacks
 *******************************************************************************/

static char *config_last_key = NULL;

static int config_string_cb(void *params, const unsigned char *val, size_t len) {
    if (key_is(config_last_key, "config")) {
        fwrite(val, 1, len, stdout);
    }
    return 1;
}

static int config_map_key_cb(void *params, const unsigned char *keyVal, size_t keyLen) {
    remember_key(&config_last_key, keyVal, keyLen);
    return 1;
}

static yajl_callbacks config_callbacks = {
    .yajl_string = config_string_cb,
    .yajl_map_key = config_map_key_cb,
};

static void print_usage(void) {
    printf("i3-msg " I3_VERSION "\n");
    printf("i3-msg [-q] [-v] [-h] [-s <socket>] [-t <type>] [-m] [-r] <message>\n");
}

int main(int argc, char *argv[]) {
    char *socket_path = NULL;
    int o;
    uint32_t message_type = I3_IPC_MESSAGE_TYPE_RUN_COMMAND;
    char *payload = NULL;
    bool quiet = false;
    bool monitor = false;
    bool raw_reply = false;

    static const struct option long_options[] = {
        {"socket", required_argument, 0, 's'},
        {"type", required_argument, 0, 't'},
        {"version", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"monitor", no_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {"raw", no_argument, 0, 'r'},
        {0, 0, 0, 0}};

    const char *options_string = "s:t:vhqmr";

    while ((o = getopt_long(argc, argv, options_string, long_options, NULL)) != -1) {
        if (o == 's') {
            free(socket_path);
            socket_path = sstrdup(optarg);
        } else if (o == 't') {
            if (!lookup_message_type(optarg, &message_type)) {
                fprintf(stderr, "Unknown message type\n");
                fprintf(stderr, "Known types: ");
                bool first = true;
                for (size_t i = 0; i < sizeof(message_types) / sizeof(message_types[0]); i++) {
                    if (message_types[i].alias) {
                        continue;
                    }
                    fprintf(stderr, "%s%s", (first ? "" : ", "), message_types[i].name);
                    first = false;
                }
                fprintf(stderr, "\n");
                exit(EXIT_FAILURE);
            }
        } else if (o == 'q') {
            quiet = true;
        } else if (o == 'm') {
            monitor = true;
        } else if (o == 'v') {
            printf("i3-msg " I3_VERSION "\n");
            return 0;
        } else if (o == 'h') {
            print_usage();
            return 0;
        } else if (o == '?') {
            exit(EXIT_FAILURE);
        } else if (o == 'r') {
            raw_reply = true;
        }
    }

    if (monitor && message_type != I3_IPC_MESSAGE_TYPE_SUBSCRIBE) {
        fprintf(stderr, "The monitor option -m is used with -t SUBSCRIBE exclusively.\n");
        exit(EXIT_FAILURE);
    }

    /* Use all arguments, separated by whitespace, as payload.
     * This way, you don’t have to do i3-msg 'mark foo', you can use
     * i3-msg mark foo */
    while (optind < argc) {
        if (!payload) {
            payload = sstrdup(argv[optind]);
        } else {
            char *both;
            sasprintf(&both, "%s %s", payload, argv[optind]);
            free(payload);
            payload = both;
        }
        optind++;
    }

    if (!payload) {
        payload = sstrdup("");
    }

    int sockfd = ipc_connect(socket_path);
    if (ipc_send_message(sockfd, strlen(payload), message_type, (uint8_t *)payload) == -1) {
        err(EXIT_FAILURE, "IPC: write()");
    }
    free(payload);

    uint32_t reply_length;
    uint32_t reply_type;
    uint8_t *reply;
    recv_message_or_die(sockfd, &reply_type, &reply_length, &reply);
    if (reply_type != message_type) {
        errx(EXIT_FAILURE, "IPC: Received reply of type %u but expected %u", reply_type, message_type);
    }
    /* For the reply of commands, have a look if that command was successful.
     * If not, nicely format the error message. */
    if (reply_type == I3_IPC_REPLY_TYPE_COMMAND) {
        if (!raw_reply) {
            parse_json_or_die(&reply_callbacks, reply, reply_length);
        }

        if (!quiet || raw_reply) {
            print_reply(stdout, reply, reply_length);
        }
    } else if (reply_type == I3_IPC_REPLY_TYPE_CONFIG) {
        if (raw_reply) {
            print_reply(stdout, reply, reply_length);
        } else {
            parse_json_or_die(&config_callbacks, reply, reply_length);
        }
    } else if (reply_type == I3_IPC_REPLY_TYPE_SUBSCRIBE) {
        /* The first reply acknowledges (or rejects) the subscription. If i3
         * rejected it, no event will ever follow, so do not wait for one. */
        parse_json_or_die(&reply_callbacks, reply, reply_length);
        free(reply);
        if (exit_code != 0) {
            close(sockfd);
            return exit_code;
        }

        do {
            recv_message_or_die(sockfd, &reply_type, &reply_length, &reply);

            if (!(reply_type & I3_IPC_EVENT_MASK)) {
                errx(EXIT_FAILURE, "IPC: Received reply of type %u but expected an event", reply_type);
            }

            if (!quiet) {
                print_reply(stdout, reply, reply_length);
                fflush(stdout);
            }
            free(reply);
        } while (monitor);

        /* All replies were freed above. */
        reply = NULL;
    } else {
        if (!quiet) {
            print_reply(stdout, reply, reply_length);
        }
    }

    free(reply);

    close(sockfd);

    free(last_key);
    free(config_last_key);
    reply_reset();
    free(socket_path);

    return exit_code;
}
