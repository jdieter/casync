#include <curl/curl.h>
#include <getopt.h>

#include "caprotocol.h"
#include "caremote.h"
#include "realloc-buffer.h"
#include "util.h"

static bool arg_verbose = false;

typedef enum ProcessUntil {
        PROCESS_UNTIL_CAN_PUT_CHUNK,
        PROCESS_UNTIL_CAN_PUT_INDEX,
        PROCESS_UNTIL_HAVE_REQUEST,
        PROCESS_UNTIL_FINISHED,
} ProcessUntil;

static int process_remote(CaRemote *rr, ProcessUntil until) {
        int r;

        assert(rr);

        for (;;) {

                switch (until) {

                case PROCESS_UNTIL_CAN_PUT_CHUNK:

                        r = ca_remote_can_put_chunk(rr);
                        if (r == -EPIPE)
                                return r;
                        if (r < 0) {
                                fprintf(stderr, "Failed to determine whether we can add a chunk to the buffer: %s\n", strerror(-r));
                                return r;
                        }
                        if (r > 0)
                                return 0;

                        break;

                case PROCESS_UNTIL_CAN_PUT_INDEX:

                        r = ca_remote_can_put_index(rr);
                        if (r == -EPIPE)
                                return r;
                        if (r < 0) {
                                fprintf(stderr, "Failed to determine whether we can add an index fragment to the buffer: %s\n", strerror(-r));
                                return r;
                        }
                        if (r > 0)
                                return 0;

                        break;

                case PROCESS_UNTIL_HAVE_REQUEST:

                        r = ca_remote_has_pending_requests(rr);
                        if (r == -EPIPE)
                                return r;
                        if (r < 0) {
                                fprintf(stderr, "Failed to determine whether there are pending requests.\n");
                                return r;
                        }
                        if (r > 0)
                                return 0;

                        break;

                case PROCESS_UNTIL_FINISHED:
                        break;

                default:
                        assert(false);
                }

                r = ca_remote_step(rr);
                if (r == -EPIPE || r == CA_REMOTE_FINISHED) {

                        if (until == PROCESS_UNTIL_FINISHED)
                                return 0;

                        return -EPIPE;
                }
                if (r < 0) {
                        fprintf(stderr, "Failed to process remoting engine: %s\n", strerror(-r));
                        return r;
                }

                if (r != CA_REMOTE_POLL)
                        continue;

                r = ca_remote_poll(rr, UINT64_MAX);
                if (r < 0) {
                        fprintf(stderr, "Failed to poll remoting engine: %s\n", strerror(-r));
                        return r;
                }
        }
}

static size_t write_index(const void *buffer, size_t size, size_t nmemb, void *userdata) {
        CaRemote *rr = userdata;
        size_t product;
        int r;

        product = size * nmemb;

        r = process_remote(rr, PROCESS_UNTIL_CAN_PUT_INDEX);
        if (r < 0)
                return 0;

        r = ca_remote_put_index(rr, buffer, product);
        if (r < 0) {
                fprintf(stderr, "Failed to put index: %s\n", strerror(-r));
                return 0;
        }

        return product;
}

static size_t write_chunk(const void *buffer, size_t size, size_t nmemb, void *userdata) {
        ReallocBuffer *chunk_buffer = userdata;
        size_t product, z;

        product = size * nmemb;

        z = realloc_buffer_size(chunk_buffer) + product;
        if (z < realloc_buffer_size(chunk_buffer)) {
                fprintf(stderr, "Overflow\n");
                return 0;
        }

        if (z > (CA_PROTOCOL_SIZE_MAX - offsetof(CaProtocolChunk, data))) {
                fprintf(stderr, "Chunk too large\n");
                return 0;
        }

        if (!realloc_buffer_append(chunk_buffer, buffer, product)) {
                log_oom();
                return 0;
        }

        return product;
}

static char *chunk_url(const char *store_url, const CaChunkID *id) {
        char ids[CA_CHUNK_ID_FORMAT_MAX], *buffer;
        size_t n;

        /* Chop off URL arguments and multiple trailing dashes, then append the chunk ID and ".xz" */

        n = strcspn(store_url, "?;");
        while (n > 0 && store_url[n-1] == '/')
                n--;

        buffer = new(char, n + 1 + 4 + 1 + CA_CHUNK_ID_FORMAT_MAX-1 + 3 + 1);

        ca_chunk_id_format(id, ids);

        strcpy(mempcpy(mempcpy(mempcpy(mempcpy(mempcpy(buffer, store_url, n), "/", 1), ids, 4), "/", 1), ids, CA_CHUNK_ID_FORMAT_MAX-1), ".xz");

        return buffer;
}

static int run(int argc, char *argv[]) {
        const char *base_url, *archive_url, *index_url, *wstore_url;
        size_t n_stores = 0, current_store = 0;
        char *url_buffer = NULL;
        CURL *curl = NULL;
        ReallocBuffer chunk_buffer = {};
        CaRemote *rr = NULL;
        long http_status;
        int r;

        if (argc < 5) {
                fprintf(stderr, "Expected at least 5 arguments.\n");
                return -EINVAL;
        }

        /* fprintf(stderr, "base=%s archive=%s index=%s wstore=%s\n", argv[1], argv[2], argv[3], argv[4]); */

        base_url = empty_or_dash_to_null(argv[1]);
        archive_url = empty_or_dash_to_null(argv[2]);
        index_url = empty_or_dash_to_null(argv[3]);
        wstore_url = empty_or_dash_to_null(argv[4]);

        n_stores = !!wstore_url + (argc - 5);

        if (base_url || archive_url) {
                fprintf(stderr, "Pushing/pulling to base or archive via HTTP not yet supported.\n");
                return -EOPNOTSUPP;
        }

        if (!index_url && n_stores == 0) {
                fprintf(stderr, "Nothing to do.\n");
                return -EINVAL;
        }

        rr = ca_remote_new();
        if (!rr) {
                r = log_oom();
                goto finish;
        }

        r = ca_remote_set_local_feature_flags(rr,
                                              (n_stores > 0 ? CA_PROTOCOL_READABLE_STORE : 0) |
                                              (index_url ? CA_PROTOCOL_READABLE_INDEX : 0));
        if (r < 0) {
                fprintf(stderr, "Failed to set feature flags: %s\n", strerror(-r));
                goto finish;
        }

        r = ca_remote_set_io_fds(rr, STDIN_FILENO, STDOUT_FILENO);
        if (r < 0) {
                fprintf(stderr, "Failed to set I/O file descriptors: %s\n", strerror(-r));
                goto finish;
        }

        curl = curl_easy_init();
        if (!curl) {
                r = log_oom();
                goto finish;
        }

        if (curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK) {
                fprintf(stderr, "Failed to turn on location following.\n");
                r = -EIO;
                goto finish;
        }

        /* (void) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); */

        if (index_url) {
                if (curl_easy_setopt(curl, CURLOPT_URL, index_url) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL URL to: %s\n", index_url);
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_index) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL callback function.\n");
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_setopt(curl, CURLOPT_WRITEDATA, rr) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL private data.\n");
                        r = -EIO;
                        goto finish;
                }

                if (arg_verbose)
                        fprintf(stderr, "Acquiring %s...\n", index_url);

                if (curl_easy_perform(curl) != CURLE_OK) {
                        fprintf(stderr, "Failed to acquire %s\n", index_url);
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK) {
                        fprintf(stderr, "Failed to query response code\n");
                        r = -EIO;
                        goto finish;
                }

                if (http_status != 200) {
                        char *m;

                        if (arg_verbose)
                                fprintf(stderr, "HTTP server failure %li while requesting %s.\n", http_status, index_url);

                        if (asprintf(&m, "HTTP request on %s failed with status %li", index_url, http_status) < 0) {
                                r = log_oom();
                                goto finish;
                        }

                        (void) ca_remote_abort(rr, http_status == 404 ? ENOMEDIUM : EBADR, m);
                        free(m);
                        goto flush;
                }

                r = process_remote(rr, PROCESS_UNTIL_CAN_PUT_INDEX);
                if (r < 0)
                        goto finish;

                r = ca_remote_put_index_eof(rr);
                if (r < 0) {
                        fprintf(stderr, "Failed to put index EOF: %s\n", strerror(-r));
                        goto finish;
                }
        }

        for (;;) {
                const char *store_url;
                CaChunkID id;

                if (n_stores == 0)  /* No stores? Then we did all we could do */
                        break;

                r = process_remote(rr, PROCESS_UNTIL_HAVE_REQUEST);
                if (r == -EPIPE) {
                        r = 0;
                        goto finish;
                }
                if (r < 0)
                        goto finish;

                r = ca_remote_next_request(rr, &id);
                if (r < 0) {
                        fprintf(stderr, "Failed to determine next chunk to get: %s\n", strerror(-r));
                        goto finish;
                }

                current_store = current_store % n_stores;
                if (wstore_url)
                        store_url = current_store == 0 ? wstore_url : argv[current_store + 5 - 1];
                else
                        store_url = argv[current_store + 5];
                /* current_store++; */

                free(url_buffer);
                url_buffer = chunk_url(store_url, &id);
                if (!url_buffer) {
                        r = log_oom();
                        goto finish;
                }

                if (curl_easy_setopt(curl, CURLOPT_URL, url_buffer) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL URL to: %s\n", index_url);
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_chunk) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL callback function.\n");
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk_buffer) != CURLE_OK) {
                        fprintf(stderr, "Failed to set CURL private data.\n");
                        r = -EIO;
                        goto finish;
                }

                if (arg_verbose)
                        fprintf(stderr, "Acquiring %s...\n", url_buffer);

                if (curl_easy_perform(curl) != CURLE_OK) {
                        fprintf(stderr, "Failed to acquire %s\n", url_buffer);
                        r = -EIO;
                        goto finish;
                }

                if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK) {
                        fprintf(stderr, "Failed to query response code\n");
                        r = -EIO;
                        goto finish;
                }

                r = process_remote(rr, PROCESS_UNTIL_CAN_PUT_CHUNK);
                if (r == -EPIPE) {
                        r = 0;
                        goto finish;
                }
                if (r < 0)
                        goto finish;

                if (http_status == 200) {
                        r = ca_remote_put_chunk(rr, &id, CA_CHUNK_COMPRESSED, realloc_buffer_data(&chunk_buffer), realloc_buffer_size(&chunk_buffer));
                        if (r < 0) {
                                fprintf(stderr, "Failed to write chunk: %s\n", strerror(-r));
                                goto finish;
                        }
                } else {
                        if (arg_verbose)
                                fprintf(stderr, "HTTP server failure %li while requesting %s.\n", http_status, url_buffer);

                        r = ca_remote_put_missing(rr, &id);
                        if (r < 0) {
                                fprintf(stderr, "Failed to write missing message: %s\n", strerror(-r));
                                goto finish;
                        }
                }

                realloc_buffer_empty(&chunk_buffer);
        }

flush:
        r = process_remote(rr, PROCESS_UNTIL_FINISHED);

finish:
        if (curl)
                curl_easy_cleanup(curl);

        free(url_buffer);
        realloc_buffer_free(&chunk_buffer);

        ca_remote_unref(rr);

        return r;
}

static void help(void) {
        printf("%s -- casync HTTP helper. Do not execute manually.\n", program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        static const struct option options[] = {
                { "help",           no_argument,       NULL, 'h'                },
                { "verbose",        no_argument,       NULL, 'v'                },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        if (getenv_bool("CASYNC_VERBOSE") > 0)
                arg_verbose = true;

        while ((c = getopt_long(argc, argv, "hv", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case 'v':
                        arg_verbose = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert(false);
                }
        }

        return 1;
}

int main(int argc, char* argv[]) {
        static const struct sigaction sa = {
                .sa_handler = SIG_IGN,
                .sa_flags = SA_RESTART,
        };

        int r;

        assert_se(sigaction(SIGPIPE, &sa, NULL) >= 0);

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (argc < 2) {
                fprintf(stderr, "Verb expected.\n");
                r = -EINVAL;
                goto finish;
        }

        if (streq(argv[1], "pull"))
                r = run(argc-1, argv+1);
        else {
                fprintf(stderr, "Unknown verb: %s\n", argv[1]);
                r = -EINVAL;
        }

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
