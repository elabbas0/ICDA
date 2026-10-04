#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    char name[64];
    printf("Hello from an ICDA program! argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("What is your name? ");
    if (fgets(name, sizeof(name), stdin)) printf("Nice to meet you, %s", name);
    return EXIT_SUCCESS;
}
