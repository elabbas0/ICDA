#include <stdio.h>
int main(void) {
    static unsigned char buf[1 << 20];
    for (long block = 0; block < 40; block++) {
        for (long i = 0; i < (1 << 20); i++) {
            long pos = block * (1 << 20) + i;
            buf[i] = (unsigned char)((pos * 7 + 3) & 0xFF);
        }
        fwrite(buf, 1, sizeof(buf), stdout);
    }
    return 0;
}
