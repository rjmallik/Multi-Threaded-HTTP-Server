#include "helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "queue.h"
#include "request.h"
#include "response.h"
#include "rwlock.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define DEFAULT_THREADS 4
#define QUEUE_SIZE      128
#define HASH_TABLE_SIZE 256

/* Global variables */
queue_t *request_queue;
pthread_t *worker_threads;
pthread_mutex_t audit_log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* File lock structure */
typedef struct file_lock {
    char *uri;
    rwlock_t *rwlock;
    struct file_lock *next;
} file_lock_t;

file_lock_t *file_lock_table[HASH_TABLE_SIZE] = { NULL };

/* Function declarations */
unsigned int hash_uri(const char *uri);
rwlock_t *get_or_create_file_lock(const char *uri);
void *worker_thread_function(void *arg);
void handle_connection(int connfd);
void handle_get(conn_t *conn);
void handle_put(conn_t *conn);
void handle_unsupported(conn_t *conn);
void log_audit(const char *method, const char *uri, int status_code, const char *request_id);

int main(int argc, char **argv) {
    int opt, num_threads = DEFAULT_THREADS;

    /* Parse command-line arguments */
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't') {
            num_threads = atoi(optarg);
            if (num_threads <= 0) {
                fprintf(stderr, "Invalid number of threads\n");
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Port number is required\n");
        return EXIT_FAILURE;
    }

    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        fprintf(stderr, "Invalid port\n");
        return EXIT_FAILURE;
    }

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return EXIT_FAILURE;
    }

    /* Initialize resources */
    signal(SIGPIPE, SIG_IGN);
    request_queue = queue_new(QUEUE_SIZE);
    worker_threads = malloc(sizeof(pthread_t) * num_threads);

    /* Create worker threads */
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&worker_threads[i], NULL, worker_thread_function, NULL);
    }

    Listener_Socket sock;
    if (listener_init(&sock, port) < 0) {
        fprintf(stderr, "Could not initialize listener socket\n");
        return EXIT_FAILURE;
    }

    /* Dispatcher loop */
    while (1) {
        int connfd = listener_accept(&sock);
        if (connfd < 0) {
            perror("Accept failed");
            continue;
        }

        if (!queue_push(request_queue, (void *) (intptr_t) connfd)) {
            fprintf(stderr, "Request queue is full, dropping connection\n");
            close(connfd);
        }
    }

    /* Cleanup */
    close(sock.fd);
    for (int i = 0; i < num_threads; i++) {
        pthread_cancel(worker_threads[i]);
        pthread_join(worker_threads[i], NULL);
    }
    free(worker_threads);
    queue_delete(&request_queue);

    return EXIT_SUCCESS;
}

void *worker_thread_function(void *arg) {
    (void) arg; // Unused
    while (1) {
        int connfd;
        if (!queue_pop(request_queue, (void **) &connfd)) {
            continue;
        }
        handle_connection(connfd);
        close(connfd);
    }
    return NULL;
}

void handle_connection(int connfd) {
    conn_t *conn = conn_new(connfd);
    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        const Request_t *req_type = conn_get_request(conn);

        if (req_type == &REQUEST_GET) {
            handle_get(conn);
        } else if (req_type == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }

    conn_delete(&conn);
}

void handle_get(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    rwlock_t *rwlock = get_or_create_file_lock(uri);

    reader_lock(rwlock);

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        int status_code = (errno == EACCES) ? 403 : (errno == ENOENT) ? 404 : 500;
        log_audit("GET", uri, status_code, conn_get_header(conn, "Request-Id"));
        const Response_t *res = (status_code == 403)   ? &RESPONSE_FORBIDDEN
                                : (status_code == 404) ? &RESPONSE_NOT_FOUND
                                                       : &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
    } else {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            log_audit("GET", uri, 200, conn_get_header(conn, "Request-Id"));
            conn_send_file(conn, fd, st.st_size);
        } else {
            log_audit("GET", uri, 403, conn_get_header(conn, "Request-Id"));
            conn_send_response(conn, &RESPONSE_FORBIDDEN);
        }
        close(fd);
    }

    reader_unlock(rwlock);
}

void handle_put(conn_t *conn) {
    char *uri = conn_get_uri(conn);

    // Step 1: Save the message body into a temporary file
    char temp_file[] = "/tmp/httpserver_put_XXXXXX";
    int temp_fd = mkstemp(temp_file);
    if (temp_fd < 0) {
        log_audit("PUT", uri, 500, conn_get_header(conn, "Request-Id"));
        conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
        return;
    }

    // Receive the file content into the temporary file
    const Response_t *recv_res = conn_recv_file(conn, temp_fd);
    if (recv_res != NULL) {
        log_audit("PUT", uri, response_get_code(recv_res), conn_get_header(conn, "Request-Id"));
        conn_send_response(conn, recv_res);
        close(temp_fd);
        unlink(temp_file); // Clean up temporary file
        return;
    }
    close(temp_fd);

    // Step 2: Acquire writer lock after buffering the message body
    rwlock_t *rwlock = get_or_create_file_lock(uri);
    writer_lock(rwlock);

    // Step 3: Open the target file for writing
    int exists = access(uri, F_OK) == 0; // Check if the file exists
    int fd = open(uri, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        int status_code = (errno == EACCES || errno == EISDIR) ? 403 : 500;
        log_audit("PUT", uri, status_code, conn_get_header(conn, "Request-Id"));
        const Response_t *res
            = (status_code == 403) ? &RESPONSE_FORBIDDEN : &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        writer_unlock(rwlock);
        unlink(temp_file); // Clean up temporary file
        return;
    }

    // Step 4: Write the buffered content from the temporary file to the target file
    temp_fd = open(temp_file, O_RDONLY);
    if (temp_fd < 0) {
        log_audit("PUT", uri, 500, conn_get_header(conn, "Request-Id"));
        conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
        close(fd);
        writer_unlock(rwlock);
        unlink(temp_file); // Clean up temporary file
        return;
    }

    char buffer[4096];
    ssize_t bytes_read, bytes_written;
    while ((bytes_read = read(temp_fd, buffer, sizeof(buffer))) > 0) {
        bytes_written = write(fd, buffer, bytes_read);
        if (bytes_written < 0) {
            log_audit("PUT", uri, 500, conn_get_header(conn, "Request-Id"));
            conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
            close(fd);
            close(temp_fd);
            writer_unlock(rwlock);
            unlink(temp_file); // Clean up temporary file
            return;
        }
    }

    close(temp_fd);
    close(fd);

    // Step 5: Respond to the client
    if (!exists) {
        log_audit("PUT", uri, 201, conn_get_header(conn, "Request-Id")); // New file created
        conn_send_response(conn, &RESPONSE_CREATED);
    } else {
        log_audit("PUT", uri, 200, conn_get_header(conn, "Request-Id")); // Existing file updated
        conn_send_response(conn, &RESPONSE_OK);
    }

    // Step 6: Release the writer lock and clean up
    writer_unlock(rwlock);
    unlink(temp_file); // Clean up temporary file
}

void handle_unsupported(conn_t *conn) {
    log_audit("UNSUPPORTED", "", 501, conn_get_header(conn, "Request-Id"));
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void log_audit(const char *method, const char *uri, int status_code, const char *request_id) {
    pthread_mutex_lock(&audit_log_mutex);
    if (!request_id)
        request_id = "0";
    fprintf(stderr, "%s,%s,%d,%s\n", method, uri, status_code, request_id);
    pthread_mutex_unlock(&audit_log_mutex);
}

unsigned int hash_uri(const char *uri) {
    unsigned int hash = 0;
    while (*uri) {
        hash = (hash << 5) + *uri++;
    }
    return hash % HASH_TABLE_SIZE;
}

rwlock_t *get_or_create_file_lock(const char *uri) {
    unsigned int index = hash_uri(uri);
    pthread_mutex_lock(&file_lock_table_mutex);
    file_lock_t *entry = file_lock_table[index];

    while (entry) {
        if (strcmp(entry->uri, uri) == 0) {
            pthread_mutex_unlock(&file_lock_table_mutex);
            return entry->rwlock;
        }
        entry = entry->next;
    }

    file_lock_t *new_lock = malloc(sizeof(file_lock_t));
    new_lock->uri = strdup(uri);
    new_lock->rwlock = rwlock_new(READERS, 0);
    new_lock->next = file_lock_table[index];
    file_lock_table[index] = new_lock;
    pthread_mutex_unlock(&file_lock_table_mutex);
    return new_lock->rwlock;
}
