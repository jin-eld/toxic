/*  main.c
 *
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef SIGWINCH
#define SIGWINCH 28
#endif

#include <curses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <tox/tox.h>

#include "configdir.h"
#include "toxic.h"
#include "windows.h"
#include "friendlist.h"
#include "prompt.h"
#include "misc_tools.h"
#include "file_senders.h"
#include "line_info.h"
#include "settings.h"

#ifdef _SUPPORT_AUDIO
#include "audio_call.h"
#endif /* _SUPPORT_AUDIO */

#ifndef PACKAGE_DATADIR
#define PACKAGE_DATADIR "."
#endif

#ifdef _SUPPORT_AUDIO
ToxAv *av;
#endif /* _SUPPORT_AUDIO */

/* Export for use in Callbacks */
char *DATA_FILE = NULL;
ToxWindow *prompt = NULL;

struct arg_opts {
    int ignore_data_file;
    int use_ipv4;
    char config_path[MAX_STR_SIZE];
    char nodes_path[MAX_STR_SIZE];
} arg_opts;

struct _Winthread Winthread;
struct user_settings *user_settings = NULL;

static void ignore_SIGINT(int sig)
{
    return;
}

static void init_term(void)
{
    signal(SIGWINCH, on_window_resize);

#if HAVE_WIDECHAR

    if (setlocale(LC_ALL, "") == NULL) {
        fprintf(stderr, "Could not set your locale, plese check your locale settings or"
                "disable wide char support\n");
        exit(EXIT_FAILURE);
    }

#endif

    initscr();
    cbreak();
    keypad(stdscr, 1);
    noecho();
    timeout(100);

    if (has_colors()) {
        short bg_color = COLOR_BLACK;
        start_color();

        if (user_settings->colour_theme == NATIVE_COLS) {
            if (assume_default_colors(-1, -1) == OK)
                bg_color = -1;
        }

        init_pair(0, COLOR_WHITE, COLOR_BLACK);
        init_pair(1, COLOR_GREEN, bg_color);
        init_pair(2, COLOR_CYAN, bg_color);
        init_pair(3, COLOR_RED, bg_color);
        init_pair(4, COLOR_BLUE, bg_color);
        init_pair(5, COLOR_YELLOW, bg_color);
        init_pair(6, COLOR_MAGENTA, bg_color);
        init_pair(7, COLOR_BLACK, COLOR_BLACK);
        init_pair(8, COLOR_BLACK, COLOR_WHITE);
    }

    refresh();
}

static Tox *init_tox(int ipv4)
{
    /* Init core */
    int ipv6 = !ipv4;
    Tox *m = tox_new(ipv6);

    /*
    * TOX_ENABLE_IPV6_DEFAULT is always 1.
    * Checking it is redundant, this *should* be doing ipv4 fallback
    */
    if (ipv6 && m == NULL) {
        fprintf(stderr, "IPv6 didn't initialize, trying IPv4\n");
        m = tox_new(0);
    }

    if (ipv4)
        fprintf(stderr, "Forcing IPv4 connection\n");

    if (m == NULL)
        return NULL;

    /* Callbacks */
    tox_callback_connection_status(m, on_connectionchange, NULL);
    tox_callback_typing_change(m, on_typing_change, NULL);
    tox_callback_friend_request(m, on_request, NULL);
    tox_callback_friend_message(m, on_message, NULL);
    tox_callback_name_change(m, on_nickchange, NULL);
    tox_callback_user_status(m, on_statuschange, NULL);
    tox_callback_status_message(m, on_statusmessagechange, NULL);
    tox_callback_friend_action(m, on_action, NULL);
    tox_callback_group_invite(m, on_groupinvite, NULL);
    tox_callback_group_message(m, on_groupmessage, NULL);
    tox_callback_group_action(m, on_groupaction, NULL);
    tox_callback_group_namelist_change(m, on_group_namelistchange, NULL);
    tox_callback_file_send_request(m, on_file_sendrequest, NULL);
    tox_callback_file_control(m, on_file_control, NULL);
    tox_callback_file_data(m, on_file_data, NULL);

#ifdef __linux__
    tox_set_name(m, (uint8_t *) "Cool dude", strlen("Cool dude"));
#elif defined(__FreeBSD__)
    tox_set_name(m, (uint8_t *) "Nerd", strlen("Nerd"));
#elif defined(__APPLE__)
    tox_set_name(m, (uint8_t *) "Hipster", strlen("Hipster")); /* This used to users of other Unixes are hipsters */
#else
    tox_set_name(m, (uint8_t *) "Registered Minix user #4", strlen("Registered Minix user #4"));
#endif

    return m;
}

#define MINLINE  50 /* IP: 7 + port: 5 + key: 38 + spaces: 2 = 70. ! (& e.g. tox.im = 6) */
#define MAXLINE  256 /* Approx max number of chars in a sever line (name + port + key) */
#define MAXNODES 50
#define NODELEN (MAXLINE - TOX_CLIENT_ID_SIZE - 7)

static int  linecnt = 0;
static char nodes[MAXNODES][NODELEN];
static uint16_t ports[MAXNODES];
static uint8_t keys[MAXNODES][TOX_CLIENT_ID_SIZE];

static int nodelist_load(char *filename)
{
    if (!filename)
        return 1;

    FILE *fp = fopen(filename, "r");

    if (fp == NULL)
        return 1;

    char line[MAXLINE];

    while (fgets(line, sizeof(line), fp) && linecnt < MAXNODES) {
        if (strlen(line) > MINLINE) {
            const char *name = strtok(line, " ");
            const char *port = strtok(NULL, " ");
            const char *key_ascii = strtok(NULL, " ");

            /* invalid line */
            if (name == NULL || port == NULL || key_ascii == NULL)
                continue;

            snprintf(nodes[linecnt], sizeof(nodes[linecnt]), "%s", name);
            nodes[linecnt][NODELEN - 1] = 0;
            ports[linecnt] = htons(atoi(port));

            uint8_t *key_binary = hex_string_to_bin(key_ascii);
            memcpy(keys[linecnt], key_binary, TOX_CLIENT_ID_SIZE);
            free(key_binary);

            linecnt++;
        }
    }

    if (linecnt < 1) {
        fclose(fp);
        return 2;
    }

    fclose(fp);
    return 0;
}

int init_connection_helper(Tox *m, int line)
{
    return tox_bootstrap_from_address(m, nodes[line], TOX_ENABLE_IPV6_DEFAULT,
                                      ports[line], keys[line]);
}

/* Connects to a random DHT node listed in the DHTnodes file
 *
 * return codes:
 * 1: failed to open node file
 * 2: no line of sufficient length in node file
 * 3: failed to resolve name to IP
 * 4: nodelist file contains no acceptable line
 */
static bool srvlist_loaded = false;

#define NUM_INIT_NODES 5

int init_connection(Tox *m)
{
    if (linecnt > 0) /* already loaded nodelist */
        return init_connection_helper(m, rand() % linecnt) ? 0 : 3;

    /* only once:
     * - load the nodelist
     * - connect to "everyone" inside
     */
    if (!srvlist_loaded) {
        srvlist_loaded = true;
        int res;

        if (!arg_opts.nodes_path[0])
            res = nodelist_load(PACKAGE_DATADIR "/DHTnodes");
        else
            res = nodelist_load(arg_opts.nodes_path);

        if (linecnt < 1)
            return res;

        res = 3;
        int i;
        int n = MIN(NUM_INIT_NODES, linecnt);

        for (i = 0; i < n; ++i) {
            if (init_connection_helper(m, rand() % linecnt))
                res = 0;
        }

        return res;
    }

    /* empty nodelist file */
    return 4;
}

static void do_connection(Tox *m, ToxWindow *prompt)
{
    uint8_t msg[MAX_STR_SIZE] = {0};

    static int conn_try = 0;
    static int conn_err = 0;
    static bool dht_on = false;

    bool is_connected = tox_isconnected(m);

    if (!dht_on && !is_connected && !(conn_try++ % 100)) {
        if (!conn_err) {
            if ((conn_err = init_connection(m))) {
                snprintf(msg, sizeof(msg), "\nAuto-connect failed with error code %d", conn_err);
            }
        }
    } else if (!dht_on && is_connected) {
        dht_on = true;
        prompt_update_connectionstatus(prompt, dht_on);
        snprintf(msg, sizeof(msg), "DHT connected.");
    } else if (dht_on && !is_connected) {
        dht_on = false;
        prompt_update_connectionstatus(prompt, dht_on);
        snprintf(msg, sizeof(msg), "\nDHT disconnected. Attempting to reconnect.");
    }

    if (msg[0])
        line_info_add(prompt, NULL, NULL, NULL, msg, SYS_MSG, 0, 0);
}

static void load_friendlist(Tox *m)
{
    uint32_t i;
    uint32_t numfriends = tox_count_friendlist(m);

    for (i = 0; i < numfriends; ++i)
        friendlist_onFriendAdded(NULL, m, i, false);
}

/*
 * Store Messenger to given location
 * Return 0 stored successfully
 * Return 1 file path is NULL
 * Return 2 malloc failed
 * Return 3 opening path failed
 * Return 4 fwrite failed
 */
int store_data(Tox *m, char *path)
{
    if (arg_opts.ignore_data_file)
        return 0;

    if (path == NULL)
        return 1;

    FILE *fd;
    size_t len;
    uint8_t *buf;

    len = tox_size(m);
    buf = malloc(len);

    if (buf == NULL)
        return 2;

    tox_save(m, buf);

    fd = fopen(path, "wb");

    if (fd == NULL) {
        free(buf);
        return 3;
    }

    if (fwrite(buf, len, 1, fd) != 1) {
        free(buf);
        fclose(fd);
        return 4;
    }

    free(buf);
    fclose(fd);
    return 0;
}

static void load_data(Tox *m, char *path)
{
    if (arg_opts.ignore_data_file)
        return;

    FILE *fd;
    size_t len;
    uint8_t *buf;

    if ((fd = fopen(path, "rb")) != NULL) {
        fseek(fd, 0, SEEK_END);
        len = ftell(fd);
        fseek(fd, 0, SEEK_SET);

        buf = malloc(len);

        if (buf == NULL) {
            fclose(fd);
            endwin();
            fprintf(stderr, "malloc() failed. Aborting...\n");
            exit(EXIT_FAILURE);
        }

        if (fread(buf, len, 1, fd) != 1) {
            free(buf);
            fclose(fd);
            endwin();
            fprintf(stderr, "fread() failed. Aborting...\n");
            exit(EXIT_FAILURE);
        }

        tox_load(m, buf, len);
        load_friendlist(m);

        free(buf);
        fclose(fd);
    } else {
        int st;

        if ((st = store_data(m, path)) != 0) {
            endwin();
            fprintf(stderr, "Store messenger failed with return code: %d\n", st);
            exit(EXIT_FAILURE);
        }
    }
}

void exit_toxic(Tox *m)
{
    store_data(m, DATA_FILE);
    close_all_file_senders();
    kill_all_windows();
    log_disable(prompt->chatwin->log);
    line_info_cleanup(prompt->chatwin->hst);
    free(DATA_FILE);
    free(prompt->stb);
    free(prompt->chatwin->log);
    free(prompt->chatwin->hst);
    free(prompt->chatwin);
    free(user_settings);
#ifdef _SUPPORT_AUDIO
    terminate_audio();
#endif /* _SUPPORT_AUDIO */
    tox_kill(m);
    endwin();
    exit(EXIT_SUCCESS);
}

static void do_toxic(Tox *m, ToxWindow *prompt)
{
    pthread_mutex_lock(&Winthread.lock);

    do_connection(m, prompt);
    do_file_senders(m);
    tox_do(m);    /* main tox-core loop */

    pthread_mutex_unlock(&Winthread.lock);
}

void *thread_winref(void *data)
{
    Tox *m = (Tox *) data;

    while (true) {
        draw_active_window(m);
        refresh_inactive_windows();
    }
}

static void print_usage(void)
{
    fprintf(stderr, "usage: toxic [OPTION] [FILE ...]\n");
    fprintf(stderr, "  -f, --file           Use specified data file\n");
    fprintf(stderr, "  -x, --nodata         Ignore data file\n");
    fprintf(stderr, "  -4, --ipv4           Force IPv4 connection\n");
    fprintf(stderr, "  -c, --config         Use specified config file\n");
    fprintf(stderr, "  -n, --nodes          Use specified DHTnodes file\n");
    fprintf(stderr, "  -h, --help           Show this message and exit\n");
}

static void set_default_opts(void)
{
    arg_opts.use_ipv4 = 0;
    arg_opts.ignore_data_file = 0;
}

static void parse_args(int argc, char *argv[])
{
    set_default_opts();

    static struct option long_opts[] = {
        {"file", required_argument, 0, 'f'},
        {"nodata", no_argument, 0, 'x'},
        {"ipv4", no_argument, 0, '4'},
        {"config", required_argument, 0, 'c'},
        {"nodes", required_argument, 0, 'n'},
        {"help", no_argument, 0, 'h'},
    };

    const char *opts_str = "4xf:c:n:h";
    int opt, indexptr;

    while ((opt = getopt_long(argc, argv, opts_str, long_opts, &indexptr)) != -1) {
        switch (opt) {
            case 'f':
                DATA_FILE = strdup(optarg);
                break;

            case 'x':
                arg_opts.ignore_data_file = 1;
                break;

            case '4':
                arg_opts.use_ipv4 = 1;
                break;

            case 'c':
                snprintf(arg_opts.config_path, sizeof(arg_opts.config_path), "%s", optarg);
                break;

            case 'n':
                snprintf(arg_opts.nodes_path, sizeof(arg_opts.nodes_path), "%s", optarg);
                break;

            case 'h':
            default:
                print_usage();
                exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char *argv[])
{
    char *user_config_dir = get_user_config_dir();
    int config_err = 0;

    parse_args(argc, argv);

    /* Make sure all written files are read/writeable only by the current user. */
    umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    signal(SIGINT, ignore_SIGINT);

    config_err = create_user_config_dir(user_config_dir);

    if (DATA_FILE == NULL ) {
        if (config_err) {
            DATA_FILE = strdup("data");
        } else {
            DATA_FILE = malloc(strlen(user_config_dir) + strlen(CONFIGDIR) + strlen("data") + 1);

            if (DATA_FILE != NULL) {
                strcpy(DATA_FILE, user_config_dir);
                strcat(DATA_FILE, CONFIGDIR);
                strcat(DATA_FILE, "data");
            } else {
                endwin();
                fprintf(stderr, "malloc() failed. Aborting...\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    free(user_config_dir);

    /* init user_settings struct and load settings from conf file */
    user_settings = malloc(sizeof(struct user_settings));

    if (user_settings == NULL) {
        endwin();
        fprintf(stderr, "malloc() failed. Aborting...\n");
        exit(EXIT_FAILURE);
    }

    memset(user_settings, 0, sizeof(struct user_settings));

    char *p = arg_opts.config_path[0] ? arg_opts.config_path : NULL;
    int settings_err = settings_load(user_settings, p);

    Tox *m = init_tox(arg_opts.use_ipv4);
    init_term();

    if (m == NULL) {
        endwin();
        fprintf(stderr, "Failed to initialize network. Aborting...\n");
        exit(EXIT_FAILURE);
    }

    if (!arg_opts.ignore_data_file)
        load_data(m, DATA_FILE);

    prompt = init_windows(m);

    /* create new thread for ncurses stuff */
    if (pthread_mutex_init(&Winthread.lock, NULL) != 0) {
        endwin();
        fprintf(stderr, "Mutex init failed. Aborting...\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&Winthread.tid, NULL, thread_winref, (void *) m) != 0) {
        endwin();
        fprintf(stderr, "Thread creation failed. Aborting...\n");
        exit(EXIT_FAILURE);
    }

    uint8_t *msg;

#ifdef _SUPPORT_AUDIO

    av = init_audio(prompt, m);
    
    device_set(prompt, input, user_settings->audio_in_dev);
    device_set(prompt, output, user_settings->audio_out_dev);

    if ( errors() == NoError )
        msg = "Audio initiated with no problems.";
    else /* Get error code and stuff */
        msg = "Error initiating audio!";

    line_info_add(prompt, NULL, NULL, NULL, msg, SYS_MSG, 0, 0);

#endif /* _SUPPORT_AUDIO */

    if (config_err) {
        msg = "Unable to determine configuration directory. Defaulting to 'data' for a keyfile...";
        line_info_add(prompt, NULL, NULL, NULL, msg, SYS_MSG, 0, 0);
    }


    if (settings_err == -1) {
        msg = "Failed to load user settings";
        line_info_add(prompt, NULL, NULL, NULL, msg, SYS_MSG, 0, 0);
    }


    sort_friendlist_index();
    prompt_init_statusbar(prompt, m);

    while (true) {
        update_unix_time();
        do_toxic(m, prompt);
        usleep(10000);
    }

    return 0;
}
