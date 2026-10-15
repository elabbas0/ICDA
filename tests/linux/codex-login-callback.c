#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        int log = open("/tmp/codex-browser-probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(log, 1); dup2(log, 2); close(log);
        execl("/bin/busybox", "busybox", "sh", "/bin/codex", "login", (char *)0);
        _exit(127);
    }
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_port=htons(1455), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    int fd = -1, ok = 0;
    for (int i=0; i<600; i++) {
        fd=socket(AF_INET,SOCK_STREAM,0);
        if (fd >= 0 && connect(fd,(void *)&a,sizeof(a)) == 0) break;
        if (fd>=0) close(fd);
        fd=-1;
        usleep(50000);
    }
    if (fd>=0) {
        const char request[]="GET / HTTP/1.1\r\nHost: localhost:1455\r\nConnection: close\r\n\r\n";
        char response[256]={0};
        struct pollfd p={.fd=fd,.events=POLLIN};
        if (send(fd,request,sizeof(request)-1,0)==sizeof(request)-1 && poll(&p,1,10000)>0) {
            int n=recv(fd,response,sizeof(response)-1,MSG_DONTWAIT);
            ok=n>9 && !memcmp(response,"HTTP/1.1 ",9);
            char *end=strchr(response,'\r'); if(end)*end=0;
            if(ok) printf("Codex callback server replied: %s\n",response);
        }
        close(fd);
    }
    kill(child,SIGTERM);
    waitpid(child,0,0);
    puts(ok?"ICDA_CODEX_BROWSER_CALLBACK_PASS":"ICDA_CODEX_BROWSER_CALLBACK_FAIL");
    return ok?0:1;
}
