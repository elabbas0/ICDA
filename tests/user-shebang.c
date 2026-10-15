#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/proc/user.c"

static const char *test_script;
process_t *sched_current_process(void) { return 0; }
vfs_node_t *vfs_root(void) { return (vfs_node_t *)1; }
const char *vfs_read(vfs_node_t *cwd, const char *path, uint64_t *size) {
    (void)cwd;
    assert(!strcmp(path, "/bin/test-script"));
    *size = strlen(test_script);
    return test_script;
}

static void check(const char *script, const char *interpreter, const char *option) {
    char path[256], storage[2048]; char *argv[32]; uint64_t argc;
    test_script = script;
    assert(!build_shebang_argv("/bin/test-script", path, sizeof(path), storage, sizeof(storage), argv, &argc));
    assert(!strcmp(path, interpreter));
    assert(argc == (option ? 2U : 1U));
    if (option) {
        assert(argv[0] >= storage && argv[0] < storage + sizeof(storage));
        assert(!strcmp(argv[0], option));
    }
    assert(!strcmp(argv[argc - 1], "/bin/test-script"));
}
int main(void) {
    check("#!/bin/busybox sh\nexport VALUE=ok\n", "/bin/busybox", "sh");
    check("#!/bin/interpreter --flag\n", "/bin/interpreter", "--flag");
    check("#!/bin/sh\necho native\n", "/apps/shell.app", 0);
    check("#!/usr/bin/env sh\necho native\n", "/apps/shell.app", 0);
    puts("Shebang interpreter arguments persist in caller storage; BusyBox sh and legacy ICDA scripts passed");
    return 0;
}
