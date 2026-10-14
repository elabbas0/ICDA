#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int failed;
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000LL+t.tv_nsec/1000000;
}
static void check(const char *name, int result, int wanted, long long elapsed, int deadline) {
    int ok=result==wanted && elapsed>=deadline-15 && elapsed<deadline+150;
    failed += !ok;
    printf("wait-timeouts: %s %s result%d time%lldms\n", ok?"PASS":"FAIL", name,result,elapsed);
    fflush(stdout);
}
int main(void) {
    struct epoll_event ev;
    long long start;
    int ep=epoll_create1(0), r;
    if(ep<0) return 2;
    start=now_ms(); r=epoll_wait(ep,&ev,1,80);
    check("empty epoll",r,0,now_ms()-start,80);
    start=now_ms(); r=poll(0,0,80);
    check("empty poll",r,0,now_ms()-start,80);
    int e=eventfd(0,EFD_NONBLOCK);
    if(e<0) return 2;
    ev.events=EPOLLIN; ev.data.fd=e;
    if(epoll_ctl(ep,EPOLL_CTL_ADD,e,&ev)) return 2;
    start=now_ms(); r=epoll_wait(ep,&ev,1,80);
    check("unready eventfd epoll",r,0,now_ms()-start,80);
    close(e); close(ep);
    struct timeval tv={0,80000};
    start=now_ms(); r=select(0,0,0,0,&tv);
    check("empty select",r,0,now_ms()-start,80);
    ep=epoll_create1(0);
    int timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);
    if(timer<0 || ep<0) return 2;
    ev.events=EPOLLIN; ev.data.fd=timer;
    struct itimerspec ts={{0,0},{0,100000000}};
    if(epoll_ctl(ep,EPOLL_CTL_ADD,timer,&ev) || timerfd_settime(timer,0,&ts,0)) return 2;
    start=now_ms(); r=epoll_wait(ep,&ev,1,250);
    check("timerfd wake",r,1,now_ms()-start,100);
    close(timer); close(ep);
    return failed?1:0;
}
