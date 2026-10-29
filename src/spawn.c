#define _GNU_SOURCE

#include "spawn.h"

#include <lauxlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define I3SD_SPAWN_READ_BUDGET (64U * 1024U)
/* The status a shell reports when it cannot execute a command. */
#define I3SD_SPAWN_EXEC_FAILED_STATUS (127 << 8)

extern char **environ;

static bool make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool write_input(int fd, const char *input, size_t input_len) {
    size_t offset = 0;
    while (offset < input_len) {
        const ssize_t count = write(fd, input + offset, input_len - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static uint64_t slot_cookie(const struct app *app,
                            const struct spawned_process *process) {
    return SOURCE_SPAWN_FIRST + (uint64_t)(process - app->spawns);
}

/* Start argv with stdin from input_fd and stdout into output_fd. The child
 * leads a new process group so a timeout also reaches its descendants, and
 * gets the empty signal mask the reactor's signalfd setup replaced. */
static int start_child(char *const argv[], int input_fd, int output_fd,
                       pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        return result;
    }
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return result;
    }
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    /* The descriptors are close-on-exec; only the dup2 copies survive. */
    result = posix_spawn_file_actions_adddup2(&actions, input_fd,
                                              STDIN_FILENO);
    if (result == 0) {
        result = posix_spawn_file_actions_adddup2(&actions, output_fd,
                                                  STDOUT_FILENO);
    }
    if (result == 0) {
        result = posix_spawnattr_setflags(
            &attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    }
    if (result == 0) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result == 0) {
        result = posix_spawnattr_setsigmask(&attributes, &empty_mask);
    }
    if (result == 0) {
        result = posix_spawnp(pid, argv[0], &actions, &attributes, argv,
                              environ);
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return result;
}

bool i3sd_spawn_open(struct app *app, struct block *block, char *const argv[],
                     const char *input, size_t input_len, size_t output_limit,
                     uint64_t deadline_ns, int callback_ref, uint64_t serial) {
    struct spawned_process *process = NULL;
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        if (!app->spawns[index].active) {
            process = &app->spawns[index];
            break;
        }
    }
    if (process == NULL) {
        return false;
    }
    /* A private input fd keeps bounded input writes off the child pipe. */
    const int input_fd = memfd_create("i3sd-spawn-input", MFD_CLOEXEC);
    if (input_fd < 0 || !write_input(input_fd, input, input_len) ||
        lseek(input_fd, 0, SEEK_SET) < 0) {
        if (input_fd >= 0) {
            close(input_fd);
        }
        return false;
    }
    int output_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) < 0) {
        close(input_fd);
        return false;
    }
    pid_t pid = 0;
    const int spawn_error =
        start_child(argv, input_fd, output_pipe[1], &pid);
    close(input_fd);
    close(output_pipe[1]);
    if (spawn_error == EAGAIN || spawn_error == ENOMEM) {
        /* Resource exhaustion is a setup failure the caller may retry. */
        close(output_pipe[0]);
        return false;
    }

    *process = (struct spawned_process){
        .output_fd = -1,
        .block = block,
        .callback_ref = callback_ref,
        .serial = serial,
        .output_limit = output_limit,
        .output = process->output,
        .active = true,
    };
    i3sd_buffer_clear(&process->output);
    if (spawn_error != 0) {
        /* posix_spawnp reports exec failures directly. Keep the fork/exec
         * contract of an asynchronous completion with exit status 127. */
        close(output_pipe[0]);
        process->wait_status = I3SD_SPAWN_EXEC_FAILED_STATUS;
        process->exited = true;
        process->output_closed = true;
        process->finish_pending = true;
        return true;
    }
    process->pid = pid;
    process->deadline_ns = deadline_ns;
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLERR | EPOLLHUP,
        .data.u64 = slot_cookie(app, process),
    };
    if (!make_nonblocking(output_pipe[0]) ||
        epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, output_pipe[0], &event) < 0) {
        close(output_pipe[0]);
        kill(-pid, SIGTERM);
        /* SIGCHLD reaps the child; the slot is released without callback. */
        process->block = NULL;
        process->callback_ref = LUA_NOREF;
        process->active = false;
        return false;
    }
    process->output_fd = output_pipe[0];
    return true;
}

static void release_slot(struct app *app, struct spawned_process *process) {
    if (process->output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, process->output_fd, NULL);
        close(process->output_fd);
        process->output_fd = -1;
    }
    process->block = NULL;
    process->callback_ref = LUA_NOREF;
    process->active = false;
    process->finish_pending = false;
    i3sd_buffer_clear(&process->output);
}

static void cancel_process(struct app *app, struct spawned_process *process) {
    if (process->pid > 0) {
        /* The whole group stops; SIGCHLD later reaps the leader. */
        kill(-process->pid, SIGTERM);
        process->pid = 0;
    }
    luaL_unref(process->block->generation->lua, LUA_REGISTRYINDEX,
               process->callback_ref);
    release_slot(app, process);
}

void i3sd_spawn_cancel_serial(struct app *app, uint64_t serial) {
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        struct spawned_process *process = &app->spawns[index];
        if (process->active && process->serial == serial) {
            cancel_process(app, process);
            return;
        }
    }
}

void i3sd_spawn_cancel_owned(struct app *app,
                             const struct generation *generation,
                             const struct block *block) {
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        struct spawned_process *process = &app->spawns[index];
        if (process->active &&
            (block != NULL ? process->block == block
                           : process->block->generation == generation)) {
            cancel_process(app, process);
        }
    }
}

static void finish_process(struct app *app, struct spawned_process *process) {
    if (!process->active || !process->output_closed || !process->exited) {
        return;
    }

    struct block *block = process->block;
    lua_State *lua = block->generation->lua;
    const int callback_ref = process->callback_ref;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, callback_ref);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
    lua_newtable(lua);
    lua_pushlstring(lua,
                    process->output.data == NULL ? "" : process->output.data,
                    process->output.len);
    lua_setfield(lua, -2, "stdout");
    const bool exited_normally = WIFEXITED(process->wait_status);
    const bool success = exited_normally &&
                         WEXITSTATUS(process->wait_status) == 0 &&
                         !process->overflow && !process->io_error &&
                         !process->timed_out;
    lua_pushboolean(lua, success);
    lua_setfield(lua, -2, "success");
    lua_pushboolean(lua, process->overflow);
    lua_setfield(lua, -2, "overflow");
    lua_pushboolean(lua, process->io_error);
    lua_setfield(lua, -2, "io_error");
    lua_pushboolean(lua, process->timed_out);
    lua_setfield(lua, -2, "timed_out");
    if (exited_normally) {
        lua_pushinteger(lua, WEXITSTATUS(process->wait_status));
        lua_setfield(lua, -2, "exit_status");
    } else if (WIFSIGNALED(process->wait_status)) {
        lua_pushinteger(lua, WTERMSIG(process->wait_status));
        lua_setfield(lua, -2, "signal");
    }

    /* Release the slot before the callback so it may start another child. */
    release_slot(app, process);
    if (app->current == block->generation && !block->faulted) {
        if (lua_pcall(lua, 2, 0, 0) != 0) {
            i3sd_log_lua_error(block, "spawn callback");
            i3sd_fault_block(block);
        }
    } else {
        lua_pop(lua, 3);
    }
    luaL_unref(lua, LUA_REGISTRYINDEX, callback_ref);
}

void i3sd_spawn_reaped(struct app *app, pid_t pid, int wait_status) {
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        struct spawned_process *process = &app->spawns[index];
        if (process->active && process->pid == pid) {
            process->pid = 0;
            process->wait_status = wait_status;
            process->exited = true;
            finish_process(app, process);
            return;
        }
    }
}

static void close_output(struct app *app, struct spawned_process *process,
                         bool io_error) {
    if (process->output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, process->output_fd, NULL);
        close(process->output_fd);
        process->output_fd = -1;
    }
    process->output_closed = true;
    process->io_error = process->io_error || io_error;
    if (io_error && process->pid > 0) {
        kill(-process->pid, SIGTERM);
    }
    finish_process(app, process);
}

void i3sd_spawn_read(struct app *app, uint64_t cookie) {
    struct spawned_process *process =
        &app->spawns[cookie - SOURCE_SPAWN_FIRST];
    if (!process->active || process->output_fd < 0) {
        return;
    }
    char bytes[4096];
    size_t consumed = 0;
    while (consumed < I3SD_SPAWN_READ_BUDGET) {
        const ssize_t count = read(process->output_fd, bytes, sizeof(bytes));
        if (count > 0) {
            consumed += (size_t)count;
            const size_t available =
                process->output_limit - process->output.len;
            const size_t retained =
                (size_t)count < available ? (size_t)count : available;
            if (retained > 0 &&
                !i3sd_buffer_append(&process->output, bytes, retained,
                                    process->output_limit)) {
                process->overflow = true;
            }
            if ((size_t)count > retained) {
                process->overflow = true;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        close_output(app, process, count < 0);
        return;
    }
}

void i3sd_spawn_expire(struct app *app, uint64_t now_ns) {
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        struct spawned_process *process = &app->spawns[index];
        if (!process->active) {
            continue;
        }
        if (process->finish_pending) {
            process->finish_pending = false;
            finish_process(app, process);
            continue;
        }
        if (process->deadline_ns == 0 || process->deadline_ns > now_ns) {
            continue;
        }
        process->deadline_ns = 0;
        process->timed_out = true;
        if (process->pid > 0) {
            kill(-process->pid, SIGKILL);
        }
        /* Descendants may still hold the pipe open; stop waiting for them.
         * The callback runs once SIGCHLD reports the killed child. */
        close_output(app, process, false);
    }
}

uint64_t i3sd_spawn_deadline(const struct app *app) {
    uint64_t deadline = UINT64_MAX;
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        const struct spawned_process *process = &app->spawns[index];
        if (!process->active) {
            continue;
        }
        if (process->finish_pending) {
            return 0;
        }
        if (process->deadline_ns != 0 && process->deadline_ns < deadline) {
            deadline = process->deadline_ns;
        }
    }
    return deadline;
}

void i3sd_spawn_shutdown(struct app *app) {
    for (size_t index = 0; index < I3SD_MAX_SPAWNS; index++) {
        struct spawned_process *process = &app->spawns[index];
        if (process->active) {
            cancel_process(app, process);
        }
        i3sd_buffer_destroy(&process->output);
    }
}
