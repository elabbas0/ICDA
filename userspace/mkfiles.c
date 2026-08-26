#include "icda_sys.h"
int main(int argc, char **argv){
    (void)argc; (void)argv;
    for(int i=0;i<50;i++){
        char path[32];
        char c0 = '0' + (i/10)%10;
        char c1 = '0' + i%10;
        path[0]='/'; path[1]='h'; path[2]='o'; path[3]='m'; path[4]='e'; path[5]='/'; path[6]='f';
        path[7]=c0; path[8]=c1; path[9]=0;
        icda_create(path);
    }
    return 0;
}
