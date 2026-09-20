#define _GNU_SOURCE

#include "spawn.h"

#include <lauxlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define I3SD_SPAWN_READ_BUDGET (64U * 1024U)

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

bool i3sd_spawn_open(struct app *app, struct block *block, char *const argv[],
                     const char *input, size_t input_len, size_t output_limit,
                     int callback_ref, uint64_t serial) {
    if (app->spawn.active) {
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
    pid_t pid = fork();
    if (pid < 0) {
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }
    if (pid == 0) {
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, NULL);
        if (dup2(input_fd, STDIN_FILENO) < 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(input_fd);
    close(output_pipe[1]);
    if (!make_nonblocking(output_pipe[0])) {
        close(output_pipe[0]);
        kill(pid, SIGTERM);
        return false;
    }
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLERR | EPOLLHUP,
        .data.u64 = SOURCE_SPAWN,
    };
    if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, output_pipe[0], &event) < 0) {
        close(output_pipe[0]);
        kill(pid, SIGTERM);
        return false;
    }
    struct spawned_process *process = &app->spawn;
    process->pid = pid;
    process->output_fd = output_pipe[0];
    process->block = block;
    process->callback_ref = callback_ref;
    process->serial = serial;
    process->output_limit = output_limit;
    process->wait_status = 0;
    process->active = true;
    process->output_closed = false;
    process->exited = false;
    process->overflow = false;
    process->io_error = false;
    i3sd_buffer_clear(&process->output);
    return true;
}

void i3sd_spawn_cancel(struct app *app) {
    struct spawned_process *process = &app->spawn;
    if (!process->active) {
        return;
    }
    if (process->output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, process->output_fd, NULL);
        close(process->output_fd);
        process->output_fd = -1;
    }
    if (process->pid > 0) {
        kill(process->pid, SIGTERM);
        process->pid = 0;
    }
    luaL_unref(process->block->generation->lua, LUA_REGISTRYINDEX,
               process->callback_ref);
    process->block = NULL;
    process->callback_ref = LUA_NOREF;
    process->active = false;
    i3sd_buffer_clear(&process->output);
}

void i3sd_spawn_finish(struct app *app) {
    struct spawned_process *process = &app->spawn;
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
                         !process->overflow && !process->io_error;
    lua_pushboolean(lua, success);
    lua_setfield(lua, -2, "success");
    lua_pushboolean(lua, process->overflow);
    lua_setfield(lua, -2, "overflow");
    lua_pushboolean(lua, process->io_error);
    lua_setfield(lua, -2, "io_error");
    if (exited_normally) {
        lua_pushinteger(lua, WEXITSTATUS(process->wait_status));
        lua_setfield(lua, -2, "exit_status");
    } else if (WIFSIGNALED(process->wait_status)) {
        lua_pushinteger(lua, WTERMSIG(process->wait_status));
        lua_setfield(lua, -2, "signal");
    }

    /* Release the slot before the callback so it may start another child. */
    process->block = NULL;
    process->callback_ref = LUA_NOREF;
    process->active = false;
    i3sd_buffer_clear(&process->output);
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

static void close_output(struct app *app, bool io_error) {
    struct spawned_process *process = &app->spawn;
    if (process->output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, process->output_fd, NULL);
        close(process->output_fd);
        process->output_fd = -1;
    }
    process->output_closed = true;
    process->io_error = process->io_error || io_error;
    if (io_error && process->pid > 0) {
        kill(process->pid, SIGTERM);
    }
    i3sd_spawn_finish(app);
}

void i3sd_spawn_read(struct app *app) {
    struct spawned_process *process = &app->spawn;
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
        close_output(app, count < 0);
        return;
    }
}
