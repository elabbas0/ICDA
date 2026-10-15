#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Companion fixture accepts 16 x 64 KiB and returns OK for each exact
 * deterministic payload. QEMU user networking reaches its host at 10.0.2.2. */
int main(int argc, char **argv) {
    int fd = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in peer = {.sin_family=AF_INET,.sin_port=htons(19080)};
    inet_pton(AF_INET,argc>1?argv[1]:"10.0.2.2",&peer.sin_addr);
    if (fd<0 || connect(fd,(void *)&peer,sizeof(peer))) { perror("tcp-upload connect"); return 2; }
    static unsigned char bytes[65536];
    for (int turn=0;turn<16;turn++) {
        for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(unsigned char)((i+turn*17)%251);
        size_t sent=0;
        while(sent<sizeof(bytes)) {
            ssize_t n=send(fd,bytes+sent,sizeof(bytes)-sent,MSG_NOSIGNAL);
            if(n<=0) { perror("tcp-upload send"); close(fd); return 1; }
            sent+=(size_t)n;
        }
        char reply[5]={0}; size_t have=0;
        while(have<3) {
            struct pollfd p={.fd=fd,.events=POLLIN};
            if(poll(&p,1,25000)<=0) { printf("TCP_UPLOAD_FAIL timeout round=%d\n",turn); close(fd); return 1; }
            ssize_t n=recv(fd,reply+have,sizeof(reply)-1-have,0);
            if(n<=0) { puts("TCP_UPLOAD_FAIL closed peer"); close(fd); return 1; }
            have+=(size_t)n;
        }
        if(strcmp(reply,"OK\n")) { printf("TCP_UPLOAD_FAIL corrupt upload round=%d\n",turn); close(fd); return 1; }
    }
    close(fd);
    puts("TCP_UPLOAD_PASS: server verified 1048576 bytes across 16 large writes");
    return 0;
}
