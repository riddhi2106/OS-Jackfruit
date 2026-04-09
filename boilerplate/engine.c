/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    const char *base_rootfs;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

typedef struct {
    char id[CONTAINER_ID_LEN];
    int fd;
    bounded_buffer_t *buffer;
} producer_args_t;

void *producer_thread(void *arg)
{
    producer_args_t *args = (producer_args_t *)arg;
    char buffer[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(args->fd, buffer, sizeof(buffer))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, args->id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        memcpy(item.data, buffer, (size_t)n);
        
        if (bounded_buffer_push(args->buffer, &item) != 0) break;
    }

    close(args->fd);
    free(args);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 1. UTS Isolation (Hostname)
    if (sethostname(config->id, strlen(config->id)) != 0) {
        perror("child: sethostname");
        return 1;
    }
    // Ensure mount namespace changes are private and don't propagate back to host
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) < 0) {
        perror("mount private");
        exit(1);
    }

    /* 
     * Task 1: Setup namespaces and chroot
     * 1. Change root to config->rootfs
     */
    if (chroot(config->rootfs) != 0) {
        perror("child: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child: chdir");
        return 1;
    }

    // 3. Mount /proc
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("child: mount proc");
        // Don't fail completely, some workloads might not need it, but log it
    }

    // 4. Redirect Logging
    if (config->log_write_fd != -1) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    // 5. Scheduling (Nice Value)
    if (nice(config->nice_value) == -1 && errno != 0) {
        perror("child: nice");
    }

    // 6. Execute Command
    char *argv[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", argv);

    // If execv returns, it failed
    perror("child: execv");
    return 1;
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *curr = ctx->containers;
    while (curr) {
        if (strcmp(curr->id, id) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *record)
{
    record->next = ctx->containers;
    ctx->containers = record;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
typedef struct {
    int client_fd;
    supervisor_ctx_t *ctx;
} client_handler_args_t;

void *client_handler_thread(void *arg)
{
    client_handler_args_t *args = (client_handler_args_t *)arg;
    int client_fd = args->client_fd;
    supervisor_ctx_t *ctx = args->ctx;
    free(args);

    control_request_t req;
    control_response_t res;
    memset(&res, 0, sizeof(res));

    if (read(client_fd, &req, sizeof(req)) != sizeof(req)) {
        res.status = -1;
        strncpy(res.message, "Invalid request", sizeof(res.message)-1);
        write(client_fd, &res, sizeof(res));
        close(client_fd);
        return NULL;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        if (find_container(ctx, req.container_id)) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "Container %s already exists", req.container_id);
        } else {
            child_config_t *config = malloc(sizeof(child_config_t));
            strncpy(config->id, req.container_id, sizeof(config->id)-1);
            strncpy(config->rootfs, req.rootfs, sizeof(config->rootfs)-1);
            strncpy(config->command, req.command, sizeof(config->command)-1);
            config->nice_value = req.nice_value;
            
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                res.status = -1;
                perror("pipe");
                free(config);
            } else {
                config->log_write_fd = pipefd[1];
                
                char *stack = malloc(STACK_SIZE);
                pid_t child_pid = clone(child_fn, stack + STACK_SIZE, 
                                        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, 
                                        config);
                
                if (child_pid < 0) {
                    res.status = -1;
                    perror("clone");
                    close(pipefd[0]);
                    close(pipefd[1]);
                    free(stack);
                    free(config);
                } else {
                    close(pipefd[1]);
                    
                    container_record_t *rec = malloc(sizeof(container_record_t));
                    memset(rec, 0, sizeof(*rec));
                    strncpy(rec->id, req.container_id, sizeof(rec->id)-1);
                    rec->host_pid = child_pid;
                    rec->started_at = time(NULL);
                    rec->state = CONTAINER_RUNNING;
                    rec->soft_limit_bytes = req.soft_limit_bytes;
                    rec->hard_limit_bytes = req.hard_limit_bytes;
                    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);
                    
                    add_container(ctx, rec);
                    
                    if (ctx->monitor_fd >= 0) {
                        register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid, 
                                              rec->soft_limit_bytes, rec->hard_limit_bytes);
                    }

                    producer_args_t *pargs = malloc(sizeof(producer_args_t));
                    strncpy(pargs->id, rec->id, sizeof(pargs->id)-1);
                    pargs->fd = pipefd[0];
                    pargs->buffer = &ctx->log_buffer;
                    
                    pthread_t pt;
                    pthread_create(&pt, NULL, producer_thread, pargs);
                    pthread_detach(pt);

                    if (req.kind == CMD_RUN) {
                        // Task 2: Block until exit
                        while (rec->state == CONTAINER_RUNNING) {
                            pthread_mutex_unlock(&ctx->metadata_lock);
                            usleep(100000); // 100ms
                            pthread_mutex_lock(&ctx->metadata_lock);
                        }
                        res.status = 0;
                        snprintf(res.message, sizeof(res.message), "Container %s exited with status %d", rec->id, rec->exit_code);
                    } else {
                        res.status = 0;
                        snprintf(res.message, sizeof(res.message), "Container %s started with PID %d", rec->id, child_pid);
                    }
                }
            }
        }
    } else if (req.kind == CMD_STOP) {
        container_record_t *rec = find_container(ctx, req.container_id);
        if (!rec) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "Container %s not found", req.container_id);
        } else if (rec->state != CONTAINER_RUNNING) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "Container %s is not running", req.container_id);
        } else {
            rec->stop_requested = 1;
            kill(rec->host_pid, SIGTERM);
            res.status = 0;
            snprintf(res.message, sizeof(res.message), "Sent SIGTERM to container %s", req.container_id);
        }
    } else if (req.kind == CMD_LOGS) {
        container_record_t *rec = find_container(ctx, req.container_id);
        char log_path[PATH_MAX];
        if (rec) strncpy(log_path, rec->log_path, sizeof(log_path)-1);
        else snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
        
        int fd = open(log_path, O_RDONLY);
        if (fd < 0) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "No logs found for %s", req.container_id);
        } else {
            res.status = 0;
            ssize_t n = read(fd, res.message, sizeof(res.message) - 1);
            if (n < 0) n = 0;
            res.message[n] = '\0';
            close(fd);
        }
    } else if (req.kind == CMD_PS) {
        res.status = 0;
        char *ptr = res.message;
        size_t remaining = sizeof(res.message);
        int n = snprintf(ptr, remaining, "%-15s %-10s %-10s %s\n", "ID", "PID", "STATUS", "STARTED");
        ptr += n; remaining -= n;
        
        container_record_t *curr = ctx->containers;
        while (curr && remaining > 0) {
            struct tm *tm_info = localtime(&curr->started_at);
            char time_buf[20];
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
            n = snprintf(ptr, remaining, "%-15s %-10d %-10s %s\n", 
                         curr->id, curr->host_pid, state_to_string(curr->state), time_buf);
            ptr += n; remaining -= n;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    write(client_fd, &res, sizeof(res));
    close(client_fd);
    return NULL;
}

static void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
    // We need to access ctx here, but signal handlers don't take it.
    // We'll use a global pointer for simplicity in this project.
}

static supervisor_ctx_t *g_ctx = NULL;

static void global_sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (g_ctx) {
            pthread_mutex_lock(&g_ctx->metadata_lock);
            container_record_t *curr = g_ctx->containers;
            while (curr) {
                if (curr->host_pid == pid) {
                    if (WIFEXITED(status)) {
                        curr->exit_code = WEXITSTATUS(status);
                        curr->state = curr->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                    } else if (WIFSIGNALED(status)) {
                        curr->exit_signal = WTERMSIG(status);
                        curr->state = (curr->exit_signal == SIGKILL && !curr->stop_requested) ? CONTAINER_KILLED : CONTAINER_STOPPED;
                    }
                    if (g_ctx->monitor_fd >= 0) unregister_from_monitor(g_ctx->monitor_fd, curr->id, curr->host_pid);
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&g_ctx->metadata_lock);
        }
    }
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.base_rootfs = rootfs;
    g_ctx = &ctx;

    // 1. Kernel Monitor
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) perror("[warn] kernel monitor device not found");

    // 2. Control Socket
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(ctx.server_fd, 10) < 0) { perror("listen"); return 1; }

    // 3. Signals
    struct sigaction sa;
    sa.sa_handler = global_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);
    mkdir(LOG_DIR, 0755);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    printf("Supervisor running. Socket: %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        client_handler_args_t *args = malloc(sizeof(client_handler_args_t));
        args->client_fd = client_fd;
        args->ctx = &ctx;
        
        pthread_t ct;
        pthread_create(&ct, NULL, client_handler_thread, args);
        pthread_detach(ct);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor");
        fprintf(stderr, "Is the supervisor running?\n");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read(fd, &res, sizeof(res)) != sizeof(res)) {
        perror("read response");
        close(fd);
        return 1;
    }

    if (res.status != 0) {
        fprintf(stderr, "Error: %s\n", res.message);
    } else {
        printf("%s\n", res.message);
    }

    close(fd);
    return res.status == 0 ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static char last_run_id[CONTAINER_ID_LEN] = "";

static void client_sig_handler(int sig)
{
    (void)sig;
    if (last_run_id[0] != '\0') {
        control_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = CMD_STOP;
        strncpy(req.container_id, last_run_id, sizeof(req.container_id)-1);
        send_control_request(&req);
    }
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    strncpy(last_run_id, req.container_id, sizeof(last_run_id)-1);
    signal(SIGINT, client_sig_handler);
    signal(SIGTERM, client_sig_handler);

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
