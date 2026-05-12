#include "icda_sys.h"

#include <stdint.h>

#define SHELL_LINE_CAP 128
#define SHELL_BUF_CAP 1024
#define SHELL_JOB_CAP 16
#define SHELL_JOB_CMD_CAP 80
#ifndef SHELL_AUTOTEST
#define SHELL_AUTOTEST 0
#endif

#define PROC_STATE_STOPPED 4
#define PROC_STATE_EXITED 5
#define PROC_STATE_REAPED 6

typedef struct {
    uint64_t pid;
    uint64_t active;
    uint64_t notified_done;
    char command[SHELL_JOB_CMD_CAP];
} shell_job_t;

static shell_job_t shell_job_table[SHELL_JOB_CAP];

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int str_has_slash(const char *s) {
    for (uint64_t i = 0; s && s[i]; i++) if (s[i] == '/') return 1;
    return 0;
}

static void append_text(char *dst, const char *src, uint64_t cap) {
    uint64_t out = str_len(dst);
    uint64_t i = 0;
    while (src && src[i] && out + 1 < cap) dst[out++] = src[i++];
    dst[out] = 0;
}

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void write_uint(uint64_t v) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0) {
        icda_write("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    icda_write(&buf[i]);
}

static int parse_uint64(const char *text, uint64_t *out) {
    uint64_t value = 0;
    uint64_t i = 0;

    if (!text || !*text || !out) return 0;
    while (text[i]) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (uint64_t)(text[i] - '0');
        i++;
    }
    *out = value;
    return 1;
}

static const char *proc_state_name(uint64_t state) {
    switch (state) {
        case 0: return "new";
        case 1: return "ready";
        case 2: return "running";
        case 3: return "blocked";
        case 4: return "stopped";
        case 5: return "exited";
        case 6: return "reaped";
        default: return "unknown";
    }
}

static shell_job_t *shell_find_job(uint64_t pid) {
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        if (shell_job_table[i].active && shell_job_table[i].pid == pid) {
            return &shell_job_table[i];
        }
    }
    return 0;
}

static shell_job_t *shell_add_job(uint64_t pid, const char *command) {
    shell_job_t *job = shell_find_job(pid);
    if (job) {
        job->notified_done = 0;
        copy_text(job->command, command, sizeof(job->command));
        return job;
    }
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        if (!shell_job_table[i].active) {
            shell_job_table[i].active = 1;
            shell_job_table[i].pid = pid;
            shell_job_table[i].notified_done = 0;
            copy_text(shell_job_table[i].command, command, sizeof(shell_job_table[i].command));
            return &shell_job_table[i];
        }
    }
    return 0;
}

static void shell_remove_job(uint64_t pid) {
    shell_job_t *job = shell_find_job(pid);
    if (!job) return;
    job->active = 0;
    job->pid = 0;
    job->notified_done = 0;
    job->command[0] = 0;
}

static void shell_poll_jobs(int notify) {
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        icda_proc_info_t info;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        if ((long)icda_proc_info(job->pid, &info) < 0) {
            job->active = 0;
            job->pid = 0;
            job->command[0] = 0;
            continue;
        }
        if (info.state == PROC_STATE_REAPED) {
            job->active = 0;
            job->pid = 0;
            job->command[0] = 0;
            continue;
        }
        if (notify && !job->notified_done && info.state == PROC_STATE_EXITED) {
            icda_write("[job ");
            write_uint(info.pid);
            icda_write("] done exit=");
            write_uint(info.exit_code);
            icda_write(" ");
            icda_write(job->command);
            icda_write("\n");
            job->notified_done = 1;
        }
    }
}

static void print_prompt(void) {
    char cwd[80];
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
        icda_write("icda:/ ");
        return;
    }
    icda_write("icda:");
    icda_write(cwd);
    icda_write(" ");
}

static void shell_help(void) {
    icda_write("user commands: help clear pid ps jobs pwd cd ls cat mkdir touch write stat sync run spawn bg fg wait waitall stop resume kill sleep yield exit\n");
}

static void shell_pwd(void) {
    char cwd[80];
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
        icda_write("/\n");
        return;
    }
    icda_write(cwd);
    icda_write("\n");
}

static void shell_ps(void) {
    char buf[SHELL_BUF_CAP];
    long ret = (long)icda_list_procs(buf, sizeof(buf));
    if (ret < 0) {
        icda_write("ps failed\n");
        return;
    }
    icda_write(buf);
}

static void shell_ls(const char *path) {
    char buf[SHELL_BUF_CAP];
    const char *target = (path && *path) ? path : ".";
    long ret = (long)icda_list_dir(target, buf, sizeof(buf));
    if (ret < 0) {
        icda_write("ls failed\n");
        return;
    }
    icda_write(buf);
}

static void shell_cat(const char *path) {
    char buf[SHELL_BUF_CAP];
    long ret;
    if (!path || !*path) {
        icda_write("usage: cat <path>\n");
        return;
    }
    ret = (long)icda_read_file(path, buf, sizeof(buf));
    if (ret < 0) {
        icda_write("cat failed\n");
        return;
    }
    icda_write(buf);
    if (ret == 0 || buf[ret - 1] != '\n') icda_write("\n");
}

static void shell_run_path(const char *path) {
    uint64_t pid;
    uint64_t code;
    if (!path || !*path) {
        icda_write("usage: run <path>\n");
        return;
    }
    pid = icda_spawn(path);
    if ((long)pid < 0) {
        icda_write("run failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        icda_write("wait failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("pid=");
    write_uint(pid);
    icda_write(" ");
    icda_write("exit=");
    write_uint(code);
    icda_write("\n");
}

static void shell_mkdir(const char *path) {
    if (!path || !*path) {
        icda_write("usage: mkdir <path>\n");
        return;
    }
    if ((long)icda_mkdir(path) < 0) {
        icda_write("mkdir failed\n");
    }
}

static void shell_touch(const char *path) {
    if (!path || !*path) {
        icda_write("usage: touch <path>\n");
        return;
    }
    if ((long)icda_create(path) < 0) {
        icda_write("touch failed\n");
    }
}

static void shell_write_file(const char *arg) {
    char *text;
    long ret;

    if (!arg || !*arg) {
        icda_write("usage: write <path> <text>\n");
        return;
    }

    text = (char *)arg;
    while (*text && *text != ' ' && *text != '\t') text++;
    if (!*text) {
        icda_write("usage: write <path> <text>\n");
        return;
    }

    *text++ = 0;
    while (*text == ' ' || *text == '\t') text++;
    ret = (long)icda_write_file(arg, text, str_len(text));
    if (ret < 0) {
        icda_write("write failed\n");
    }
}

static void shell_stat(const char *path) {
    icda_stat_t st;

    if (!path || !*path) {
        icda_write("usage: stat <path>\n");
        return;
    }
    if ((long)icda_stat(path, &st) < 0) {
        icda_write("stat failed\n");
        return;
    }

    icda_write("inode=");
    write_uint(st.inode);
    icda_write(" type=");
    icda_write(st.type == 2 ? "dir" : "file");
    icda_write(" size=");
    write_uint(st.size);
    icda_write(" created=");
    write_uint(st.created);
    icda_write(" modified=");
    write_uint(st.modified);
    icda_write(" readonly=");
    icda_write(st.readonly ? "yes" : "no");
    icda_write("\n");
}

static void shell_spawn_path(const char *path) {
    uint64_t pid;
    if (!path || !*path) {
        icda_write("usage: spawn <path>\n");
        return;
    }
    pid = icda_spawn(path);
    if ((long)pid < 0) {
        icda_write("spawn failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }
    if (!shell_add_job(pid, path)) {
        icda_write("warning: job table full, process still running\n");
    }
    icda_write("spawned pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_wait_pid(const char *arg) {
    uint64_t pid = 0;
    uint64_t code;

    if (!arg || !*arg) {
        icda_write("usage: wait <pid>\n");
        return;
    }
    if (!parse_uint64(arg, &pid)) {
        icda_write("usage: wait <pid>\n");
        return;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        icda_write("wait failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    shell_remove_job(pid);
    icda_write("pid=");
    write_uint(pid);
    icda_write(" exit=");
    write_uint(code);
    icda_write("\n");
}

static void shell_yield_once(void) {
    icda_yield();
}

static void shell_sync(void) {
    if ((long)icda_sync() < 0) {
        icda_write("sync failed\n");
        return;
    }
    icda_write("synced\n");
}

static void shell_sleep_ticks(const char *arg) {
    uint64_t ticks = 0;

    if (!arg || !*arg || !parse_uint64(arg, &ticks)) {
        icda_write("usage: sleep <ticks>\n");
        return;
    }
    icda_sleep(ticks);
}

static void shell_jobs(void) {
    int any = 0;

    shell_poll_jobs(0);
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        icda_proc_info_t info;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        if ((long)icda_proc_info(job->pid, &info) < 0) continue;

        any = 1;
        icda_write("pid=");
        write_uint(info.pid);
        icda_write(" sid=");
        write_uint(info.sid);
        icda_write(" pgid=");
        write_uint(info.pgid);
        icda_write(" state=");
        icda_write(proc_state_name(info.state));
        icda_write(" exit=");
        write_uint(info.exit_code);
        icda_write(" cmd=");
        icda_write(job->command);
        icda_write("\n");
    }

    if (!any) {
        icda_write("no background jobs\n");
    }
}

static void shell_wait_all(void) {
    int waited = 0;

    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        uint64_t code;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        code = icda_waitpid(job->pid);
        if ((long)code >= 0) {
            icda_write("pid=");
            write_uint(job->pid);
            icda_write(" exit=");
            write_uint(code);
            icda_write("\n");
        }
        shell_remove_job(job->pid);
        waited = 1;
    }

    if (!waited) {
        icda_write("no background jobs\n");
    }
}

static void shell_fg(const char *arg) {
    uint64_t pid = 0;
    icda_proc_info_t info;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: fg <pid>\n");
        return;
    }
    if ((long)icda_proc_info(pid, &info) >= 0 && info.state == PROC_STATE_STOPPED) {
        if ((long)icda_resume(pid) < 0) {
            icda_write("resume failed: ");
            write_uint(pid);
            icda_write("\n");
            return;
        }
    }
    shell_wait_pid(arg);
}

static void shell_stop_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: stop <pid>\n");
        return;
    }
    if ((long)icda_suspend(pid) < 0) {
        icda_write("stop failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("stopped pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_resume_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: resume <pid>\n");
        return;
    }
    if ((long)icda_resume(pid) < 0) {
        icda_write("resume failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("resumed pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_kill_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: kill <pid>\n");
        return;
    }
    if ((long)icda_kill(pid, 143) < 0) {
        icda_write("kill failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("killed pid=");
    write_uint(pid);
    icda_write("\n");
}

#if SHELL_AUTOTEST
static void shell_autotest(void) {
    uint64_t pid1;
    uint64_t pid2;
    uint64_t pid3;
    uint64_t code;

    icda_write("[selftest] spawn ticker x2\n");
    pid1 = icda_spawn("/apps/ticker.app");
    pid2 = icda_spawn("/apps/ticker.app");
    if ((long)pid1 < 0 || (long)pid2 < 0) {
        icda_write("[selftest] spawn failed\n");
        return;
    }
    icda_write("[selftest] pid1=");
    write_uint(pid1);
    icda_write(" pid2=");
    write_uint(pid2);
    icda_write("\n");
    shell_add_job(pid1, "/apps/ticker.app");
    shell_add_job(pid2, "/apps/ticker.app");
    icda_sleep(3);
    if ((long)icda_suspend(pid2) < 0) {
        icda_write("[selftest] suspend failed\n");
        return;
    }
    icda_write("[selftest] stopped pid2\n");
    icda_sleep(6);
    if ((long)icda_resume(pid2) < 0) {
        icda_write("[selftest] resume failed\n");
        return;
    }
    icda_write("[selftest] resumed pid2\n");
    pid3 = icda_spawn("/apps/ticker.app");
    if ((long)pid3 < 0) {
        icda_write("[selftest] spawn pid3 failed\n");
        return;
    }
    shell_add_job(pid3, "/apps/ticker.app");
    icda_sleep(1);
    if ((long)icda_kill(pid3, 143) < 0) {
        icda_write("[selftest] kill failed\n");
        return;
    }
    code = icda_waitpid(pid3);
    if ((long)code < 0) {
        icda_write("[selftest] wait pid3 failed\n");
        return;
    }
    icda_write("[selftest] pid3 exit=");
    write_uint(code);
    icda_write("\n");
    shell_remove_job(pid3);
    icda_sleep(10);
    shell_poll_jobs(1);
    shell_wait_all();
    icda_write("[selftest] done\n");
}
#endif

static int shell_try_exec_command(const char *cmd) {
    char path[160];
    uint64_t pid = icda_spawn(cmd);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }
    if (str_has_slash(cmd)) return 0;

    path[0] = 0;
    append_text(path, "/apps/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    pid = icda_spawn(path);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }

    path[0] = 0;
    append_text(path, "/bin/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    pid = icda_spawn(path);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }
    return 0;
}

static void shell_dispatch(char *line) {
    char *arg = 0;
    while (*line == ' ' || *line == '\t') line++;
    for (uint64_t i = 0; line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            line[i] = 0;
            arg = &line[i + 1];
            while (*arg == ' ' || *arg == '\t') arg++;
            break;
        }
    }
    if (*line == 0) return;
    if (str_eq(line, "help")) { shell_help(); return; }
    if (str_eq(line, "clear")) { icda_clear(); return; }
    if (str_eq(line, "pid")) { icda_write("pid="); write_uint(icda_get_pid()); icda_write("\n"); return; }
    if (str_eq(line, "ps")) { shell_ps(); return; }
    if (str_eq(line, "jobs")) { shell_jobs(); return; }
    if (str_eq(line, "pwd")) { shell_pwd(); return; }
    if (str_eq(line, "cd")) { if ((long)icda_chdir((arg && *arg) ? arg : "/") < 0) icda_write("cd failed\n"); return; }
    if (str_eq(line, "ls")) { shell_ls(arg); return; }
    if (str_eq(line, "cat")) { shell_cat(arg); return; }
    if (str_eq(line, "mkdir")) { shell_mkdir(arg); return; }
    if (str_eq(line, "touch")) { shell_touch(arg); return; }
    if (str_eq(line, "write")) { shell_write_file(arg); return; }
    if (str_eq(line, "stat")) { shell_stat(arg); return; }
    if (str_eq(line, "sync")) { shell_sync(); return; }
    if (str_eq(line, "run")) { shell_run_path(arg); return; }
    if (str_eq(line, "spawn")) { shell_spawn_path(arg); return; }
    if (str_eq(line, "bg")) { shell_spawn_path(arg); return; }
    if (str_eq(line, "fg")) { shell_fg(arg); return; }
    if (str_eq(line, "wait")) { shell_wait_pid(arg); return; }
    if (str_eq(line, "waitall")) { shell_wait_all(); return; }
    if (str_eq(line, "stop")) { shell_stop_pid(arg); return; }
    if (str_eq(line, "resume")) { shell_resume_pid(arg); return; }
    if (str_eq(line, "kill")) { shell_kill_pid(arg); return; }
    if (str_eq(line, "sleep")) { shell_sleep_ticks(arg); return; }
    if (str_eq(line, "yield")) { shell_yield_once(); return; }
    if (str_eq(line, "exit")) icda_exit(0);
    if (!shell_try_exec_command(line)) {
        icda_write("unknown command: ");
        icda_write(line);
        icda_write("\n");
    }
}

uint64_t shell_main(void) {
    char line[SHELL_LINE_CAP];
    icda_clear();
    icda_write("icda user shell\n");
    icda_write("type 'help' for commands\n\n");
#if SHELL_AUTOTEST
    shell_autotest();
#endif
    for (;;) {
        shell_poll_jobs(1);
        print_prompt();
        if ((long)icda_read_line(line, sizeof(line)) < 0) {
            continue;
        }
        shell_dispatch(line);
    }
}
