#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct termios2 saved, raw, got;
    if (ioctl(0,TCGETS2,&saved)) {perror("TCGETS2");return 1;}
    raw=saved; raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cflag=(raw.c_cflag & ~CBAUD) | BOTHER;
    raw.c_ispeed=raw.c_ospeed=115200;
    if(ioctl(0,TCSETS2,&raw)||ioctl(0,TCGETS2,&got)){perror("TCSETS2");return 1;}
    int okay=!memcmp(&raw,&got,sizeof(raw));
    if(ioctl(0,TCSETS2,&saved)){perror("restore terminal");return 1;}
    printf("Extended terminal settings roundtrip and restore: %s\n",okay?"PASS":"FAIL");
    return okay?0:1;
}
