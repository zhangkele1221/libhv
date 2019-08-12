#include "hloop.h"
#include "hevent.h"
#include "hio.h"
#include "iowatcher.h"

#include "hdef.h"
#include "hlog.h"
#include "hmath.h"
#include "hsocket.h"

#define PAUSE_TIME              10          // ms
#define MAX_BLOCK_TIME          1000        // ms

#define IO_ARRAY_INIT_SIZE      64
static void hio_init(hio_t* io);
static void hio_deinit(hio_t* io);
static void hio_reset(hio_t* io);
static void hio_free(hio_t* io);

static int timers_compare(const struct heap_node* lhs, const struct heap_node* rhs) {
    return TIMER_ENTRY(lhs)->next_timeout < TIMER_ENTRY(rhs)->next_timeout;
}

static int hloop_process_idles(hloop_t* loop) {
    int nidles = 0;
    struct list_node* node = loop->idles.next;
    hidle_t* idle = NULL;
    while (node != &loop->idles) {
        idle = IDLE_ENTRY(node);
        if (idle->repeat != INFINITE) {
            --idle->repeat;
        }
        if (idle->repeat == 0) {
            hidle_del(idle);
        }
        EVENT_PENDING(idle);
        ++nidles;
        node = node->next;
        if (!idle->active) {
            list_del(node->prev);
        }
    }
    return nidles;
}

static int hloop_process_timers(hloop_t* loop) {
    int ntimers = 0;
    htimer_t* timer = NULL;
    uint64_t now_hrtime = hloop_now_hrtime(loop);
    while (loop->timers.root) {
        timer = TIMER_ENTRY(loop->timers.root);
        if (timer->next_timeout > now_hrtime) {
            break;
        }
        if (timer->repeat != INFINITE) {
            --timer->repeat;
        }
        if (timer->repeat == 0) {
            htimer_del(timer);
        }
        EVENT_PENDING(timer);
        ++ntimers;
        heap_dequeue(&loop->timers);
        if (timer->active) {
            if (timer->event_type == HEVENT_TYPE_TIMEOUT) {
                timer->next_timeout += ((htimeout_t*)timer)->timeout*1000;
            }
            else if (timer->event_type == HEVENT_TYPE_PERIOD) {
                hperiod_t* period = (hperiod_t*)timer;
                timer->next_timeout = calc_next_timeout(period->minute, period->hour, period->day,
                        period->week, period->month) * 1e6;
            }
            heap_insert(&loop->timers, &timer->node);
        }
    }
    return ntimers;
}

static int hloop_process_ios(hloop_t* loop, int timeout) {
    int nevents = iowatcher_poll_events(loop, timeout);
    if (nevents < 0) {
        hloge("poll_events error=%d", -nevents);
    }
    return nevents < 0 ? 0 : nevents;
}

static int hloop_process_pendings(hloop_t* loop) {
    if (loop->npendings == 0) return 0;

    hevent_t* prev = NULL;
    hevent_t* next = NULL;
    int ncbs = 0;
    for (int i = HEVENT_PRIORITY_SIZE-1; i >= 0; --i) {
        next = loop->pendings[i];
        while (next) {
            if (next->pending && next->cb) {
                next->cb(next);
                ++ncbs;
            }
            prev = next;
            next = next->pending_next;
            prev->pending = 0;
            prev->pending_next = NULL;
            if (prev->destroy) {
                SAFE_FREE(prev);
            }
        }
        loop->pendings[i] = NULL;
    }
    loop->npendings = 0;
    return ncbs;
}

static int hloop_process_events(hloop_t* loop) {
    // ios -> timers -> idles
    int nios, ntimers, nidles;
    nios = ntimers = nidles = 0;

    int32_t blocktime = MAX_BLOCK_TIME;
    hloop_update_time(loop);
    if (loop->timers.root) {
        uint64_t next_min_timeout = TIMER_ENTRY(loop->timers.root)->next_timeout;
        int64_t blocktime_us = next_min_timeout - hloop_now_hrtime(loop);
        if (blocktime_us <= 0) goto process_timers;
        blocktime = blocktime_us / 1000;
        ++blocktime;
        blocktime = MIN(blocktime, MAX_BLOCK_TIME);
    }

    if (loop->nios) {
        nios = hloop_process_ios(loop, blocktime);
    }
    else {
        msleep(blocktime);
    }
    hloop_update_time(loop);

process_timers:
    if (loop->ntimers) {
        ntimers = hloop_process_timers(loop);
    }

    if (loop->npendings == 0) {
        if (loop->nidles) {
            nidles= hloop_process_idles(loop);
        }
    }
    int ncbs = hloop_process_pendings(loop);
    printd("blocktime=%d nios=%d ntimers=%d nidles=%d nactives=%d npendings=%d ncbs=%d\n",
            blocktime, nios, ntimers, nidles, loop->nactives, loop->npendings, ncbs);
    return ncbs;
}

int hloop_init(hloop_t* loop) {
    memset(loop, 0, sizeof(hloop_t));
    loop->status = HLOOP_STATUS_STOP;
    // idles
    list_init(&loop->idles);
    // timers
    heap_init(&loop->timers, timers_compare);
    // ios: init when hio_add
    //io_array_init(&loop->ios, IO_ARRAY_INIT_SIZE);
    // iowatcher: init when iowatcher_add_event
    //iowatcher_init(loop);
    // time
    time(&loop->start_time);
    loop->start_hrtime = loop->cur_hrtime = gethrtime();
    return 0;
}

void hloop_cleanup(hloop_t* loop) {
    // pendings
    printd("cleanup pendings...\n");
    for (int i = 0; i < HEVENT_PRIORITY_SIZE; ++i) {
        loop->pendings[i] = NULL;
    }
    // idles
    printd("cleanup idles...\n");
    struct list_node* node = loop->idles.next;
    hidle_t* idle;
    while (node != &loop->idles) {
        idle = IDLE_ENTRY(node);
        node = node->next;
        SAFE_FREE(idle);
    }
    list_init(&loop->idles);
    // timers
    printd("cleanup timers...\n");
    htimer_t* timer;
    while (loop->timers.root) {
        timer = TIMER_ENTRY(loop->timers.root);
        heap_dequeue(&loop->timers);
        SAFE_FREE(timer);
    }
    heap_init(&loop->timers, NULL);
    // ios
    printd("cleanup ios...\n");
    for (int i = 0; i < loop->ios.maxsize; ++i) {
        hio_t* io = loop->ios.ptr[i];
        if (io) {
            if (!(io->io_type&HIO_TYPE_STDIO)) {
                hclose(io);
            }
            hio_free(io);
        }
    }
    io_array_cleanup(&loop->ios);
    // iowatcher
    iowatcher_cleanup(loop);
}

int hloop_run(hloop_t* loop) {
    loop->loop_cnt = 0;
    loop->status = HLOOP_STATUS_RUNNING;
    while (loop->status != HLOOP_STATUS_STOP) {
        if (loop->status == HLOOP_STATUS_PAUSE) {
            msleep(PAUSE_TIME);
            hloop_update_time(loop);
            continue;
        }
        ++loop->loop_cnt;
        if (loop->nactives == 0) break;
        hloop_process_events(loop);
    }
    loop->status = HLOOP_STATUS_STOP;
    loop->end_hrtime = gethrtime();
    hloop_cleanup(loop);
    return 0;
}

int hloop_stop(hloop_t* loop) {
    loop->status = HLOOP_STATUS_STOP;
    return 0;
}

int hloop_pause(hloop_t* loop) {
    if (loop->status == HLOOP_STATUS_RUNNING) {
        loop->status = HLOOP_STATUS_PAUSE;
    }
    return 0;
}

int hloop_resume(hloop_t* loop) {
    if (loop->status == HLOOP_STATUS_PAUSE) {
        loop->status = HLOOP_STATUS_RUNNING;
    }
    return 0;
}

hidle_t* hidle_add(hloop_t* loop, hidle_cb cb, uint32_t repeat) {
    hidle_t* idle;
    SAFE_ALLOC_SIZEOF(idle);
    idle->event_type = HEVENT_TYPE_IDLE;
    idle->priority = HEVENT_LOWEST_PRIORITY;
    idle->repeat = repeat;
    list_add(&idle->node, &loop->idles);
    EVENT_ADD(loop, idle, cb);
    loop->nidles++;
    return idle;
}

void hidle_del(hidle_t* idle) {
    if (!idle->active) return;
    idle->loop->nidles--;
    EVENT_DEL(idle);
}

htimer_t* htimer_add(hloop_t* loop, htimer_cb cb, uint64_t timeout, uint32_t repeat) {
    if (timeout == 0)   return NULL;
    htimeout_t* timer;
    SAFE_ALLOC_SIZEOF(timer);
    timer->event_type = HEVENT_TYPE_TIMEOUT;
    timer->priority = HEVENT_HIGHEST_PRIORITY;
    timer->repeat = repeat;
    timer->timeout = timeout;
    hloop_update_time(loop);
    timer->next_timeout = hloop_now_hrtime(loop) + timeout*1000;
    heap_insert(&loop->timers, &timer->node);
    EVENT_ADD(loop, timer, cb);
    loop->ntimers++;
    return (htimer_t*)timer;
}

void htimer_reset(htimer_t* timer) {
    if (timer->event_type != HEVENT_TYPE_TIMEOUT || timer->pending) {
        return;
    }
    hloop_t* loop = timer->loop;
    htimeout_t* timeout = (htimeout_t*)timer;
    heap_remove(&loop->timers, &timer->node);
    timer->next_timeout = hloop_now_hrtime(loop) + timeout->timeout*1000;
    heap_insert(&loop->timers, &timer->node);
}

htimer_t* htimer_add_period(hloop_t* loop, htimer_cb cb,
                int8_t minute,  int8_t hour, int8_t day,
                int8_t week, int8_t month, uint32_t repeat) {
    if (minute > 59 || hour > 23 || day > 31 || week > 6 || month > 12) {
        return NULL;
    }
    hperiod_t* timer;
    SAFE_ALLOC_SIZEOF(timer);
    timer->event_type = HEVENT_TYPE_PERIOD;
    timer->priority = HEVENT_HIGH_PRIORITY;
    timer->repeat = repeat;
    timer->minute = minute;
    timer->hour   = hour;
    timer->day    = day;
    timer->month  = month;
    timer->week   = week;
    timer->next_timeout = calc_next_timeout(minute, hour, day, week, month) * 1e6;
    heap_insert(&loop->timers, &timer->node);
    EVENT_ADD(loop, timer, cb);
    loop->ntimers++;
    return (htimer_t*)timer;
}

void htimer_del(htimer_t* timer) {
    if (!timer->active) return;
    timer->loop->ntimers--;
    EVENT_DEL(timer);
    // NOTE: set timer->next_timeout to handle at next loop
    timer->next_timeout = hloop_now_hrtime(timer->loop);
}

void hio_init(hio_t* io) {
    memset(io, 0, sizeof(hio_t));
    io->event_type = HEVENT_TYPE_IO;
    io->event_index[0] = io->event_index[1] = -1;
    // write_queue init when hwrite try_write failed
    //write_queue_init(&io->write_queue, 4);;
}

static void fill_io_type(hio_t* io) {
    int type = 0;
    socklen_t optlen = sizeof(int);
    int ret = getsockopt(io->fd, SOL_SOCKET, SO_TYPE, (char*)&type, &optlen);
    printd("getsockopt SO_TYPE fd=%d ret=%d type=%d errno=%d\n", io->fd, ret, type, socket_errno());
    if (ret == 0) {
        switch (type) {
        case SOCK_STREAM:   io->io_type = HIO_TYPE_TCP; break;
        case SOCK_DGRAM:    io->io_type = HIO_TYPE_UDP; break;
        case SOCK_RAW:      io->io_type = HIO_TYPE_IP;  break;
        default: io->io_type = HIO_TYPE_SOCKET;         break;
        }
    }
    else if (socket_errno() == ENOTSOCK) {
        switch (io->fd) {
        case 0: io->io_type = HIO_TYPE_STDIN;   break;
        case 1: io->io_type = HIO_TYPE_STDOUT;  break;
        case 2: io->io_type = HIO_TYPE_STDERR;  break;
        default: io->io_type = HIO_TYPE_FILE;   break;
        }
    }
}

void hio_socket_init(hio_t* io) {
    // nonblocking
    nonblocking(io->fd);
    // fill io->localaddr io->peeraddr
    if (io->localaddr == NULL) {
        SAFE_ALLOC(io->localaddr, sizeof(struct sockaddr_in6));
    }
    if (io->peeraddr == NULL) {
        io->peeraddrlen = sizeof(struct sockaddr_in6);
        SAFE_ALLOC(io->peeraddr, sizeof(struct sockaddr_in6));
    }
    socklen_t addrlen = sizeof(struct sockaddr_in6);
    int ret = getsockname(io->fd, io->localaddr, &addrlen);
    printd("getsockname fd=%d ret=%d errno=%d\n", io->fd, ret, socket_errno());
    // NOTE:
    // tcp_server peeraddr set by accept
    // udp_server peeraddr set by recvfrom
    // tcp_client/udp_client peeraddr set by hio_setpeeraddr
    if (io->io_type == HIO_TYPE_TCP) {
        // tcp acceptfd
        addrlen = sizeof(struct sockaddr_in6);
        ret = getpeername(io->fd, io->peeraddr, &addrlen);
        printd("getpeername fd=%d ret=%d errno=%d\n", io->fd, ret, socket_errno());
    }
}

void hio_reset(hio_t* io) {
    fill_io_type(io);
    if (io->io_type & HIO_TYPE_SOCKET) {
        hio_socket_init(io);
    }
}

void hio_deinit(hio_t* io) {
    offset_buf_t* pbuf = NULL;
    while (!write_queue_empty(&io->write_queue)) {
        pbuf = write_queue_front(&io->write_queue);
        SAFE_FREE(pbuf->base);
        write_queue_pop_front(&io->write_queue);
    }
    write_queue_cleanup(&io->write_queue);
    io->closed = 0;
    io->accept = io->connect = io->connectex = 0;
    io->recv = io->send = 0;
    io->recvfrom = io->sendto = 0;
    io->io_type = HIO_TYPE_UNKNOWN;
    io->error = 0;
    io->events = io->revents = 0;
    io->read_cb = NULL;
    io->write_cb = NULL;
    io->close_cb = 0;
    io->accept_cb = 0;
    io->connect_cb = 0;
    io->event_index[0] = io->event_index[1] = -1;
    io->hovlp = NULL;
}

void hio_free(hio_t* io) {
    if (io == NULL) return;
    hio_deinit(io);
    SAFE_FREE(io->localaddr);
    SAFE_FREE(io->peeraddr);
    SAFE_FREE(io);
}

hio_t* hio_get(hloop_t* loop, int fd) {
    if (loop->ios.maxsize == 0) {
        io_array_init(&loop->ios, IO_ARRAY_INIT_SIZE);
    }

    if (fd >= loop->ios.maxsize) {
        int newsize = ceil2e(fd);
        io_array_resize(&loop->ios, newsize > fd ? newsize : 2*fd);
    }

    hio_t* io = loop->ios.ptr[fd];
    if (io == NULL) {
        SAFE_ALLOC_SIZEOF(io);
        hio_init(io);
        io->loop = loop;
        io->fd = fd;
        loop->ios.ptr[fd] = io;
    }

    return io;
}

int hio_add(hio_t* io, hio_cb cb, int events) {
    printd("hio_add fd=%d events=%d\n", io->fd, events);
    hloop_t* loop = io->loop;
    if (!io->active) {
        hio_reset(io);
        EVENT_ADD(loop, io, cb);
        loop->nios++;
    }

    if (cb) {
        io->cb = (hevent_cb)cb;
    }

    iowatcher_add_event(loop, io->fd, events);
    io->events |= events;
    return 0;
}

int hio_del(hio_t* io, int events) {
    printd("hio_del fd=%d io->events=%d events=%d\n", io->fd, io->events, events);
    if (!io->active) return 0;
    iowatcher_del_event(io->loop, io->fd, events);
    io->events &= ~events;
    if (io->events == 0) {
        io->loop->nios--;
        // NOTE: not EVENT_DEL, avoid free
        EVENT_INACTIVE(io);
        hio_deinit(io);
    }
    return 0;
}

void hio_setlocaladdr(hio_t* io, struct sockaddr* addr, int addrlen) {
    if (io->localaddr == NULL) {
        SAFE_ALLOC(io->localaddr, sizeof(struct sockaddr_in6));
    }
    memcpy(io->localaddr, addr, addrlen);
}

void hio_setpeeraddr (hio_t* io, struct sockaddr* addr, int addrlen) {
    if (io->peeraddr == NULL) {
        io->peeraddrlen = sizeof(struct sockaddr_in6);
        SAFE_ALLOC(io->peeraddr, sizeof(struct sockaddr_in6));
    }
    memcpy(io->peeraddr, addr, addrlen);
}

hio_t* hread(hloop_t* loop, int fd, void* buf, size_t len, hread_cb read_cb) {
    hio_t* io = hio_get(loop, fd);
    if (io == NULL) return NULL;
    io->readbuf.base = (char*)buf;
    io->readbuf.len = len;
    if (read_cb) {
        io->read_cb = read_cb;
    }
    hio_read(io);
    return io;
}

hio_t* hwrite(hloop_t* loop, int fd, const void* buf, size_t len, hwrite_cb write_cb) {
    hio_t* io = hio_get(loop, fd);
    if (io == NULL) return NULL;
    if (write_cb) {
        io->write_cb = write_cb;
    }
    hio_write(io, buf, len);
    return io;
}

void hclose(hio_t* io) {
    printd("close fd=%d\n", io->fd);
    if (io->closed) return;
    io->closed = 1;
    hio_close(io);
    if (io->close_cb) {
        printd("close_cb------\n");
        io->close_cb(io);
        printd("close_cb======\n");
    }
    hio_del(io, ALL_EVENTS);
}

hio_t* haccept(hloop_t* loop, int listenfd, haccept_cb accept_cb) {
    hio_t* io = hio_get(loop, listenfd);
    if (io == NULL) return NULL;
    io->accept = 1;
    if (accept_cb) {
        io->accept_cb = accept_cb;
    }
    hio_accept(io);
    return io;
}

hio_t* hconnect (hloop_t* loop, int connfd, hconnect_cb connect_cb) {
    hio_t* io = hio_get(loop, connfd);
    if (io == NULL) return NULL;
    io->connect = 1;
    if (connect_cb) {
        io->connect_cb = connect_cb;
    }
    hio_connect(io);
    return io;
}

hio_t* create_tcp_server (hloop_t* loop, int port, haccept_cb accept_cb) {
    int listenfd = Listen(port);
    if (listenfd < 0) {
        return NULL;
    }
    hio_t* io = haccept(loop, listenfd, accept_cb);
    if (io == NULL) {
        closesocket(listenfd);
    }
    return io;
}

hio_t* create_tcp_client (hloop_t* loop, const char* host, int port, hconnect_cb connect_cb) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, addrlen);
    addr.sin_family = AF_INET;
    int ret = Resolver(host, (struct sockaddr*)&addr);
    if (ret != 0) return NULL;
    addr.sin_port = htons(port);
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        return NULL;
    }
    hio_t* io = hio_get(loop, connfd);
    if (io == NULL) return NULL;
    hio_setpeeraddr(io, (struct sockaddr*)&addr, addrlen);
    hconnect(loop, connfd, connect_cb);
    return io;
}

hio_t* hrecv (hloop_t* loop, int connfd, void* buf, size_t len, hread_cb read_cb) {
    hio_t* io = hio_get(loop, connfd);
    if (io == NULL) return NULL;
    io->recv = 1;
    io->io_type = HIO_TYPE_TCP;
    return hread(loop, connfd, buf, len, read_cb);
}

hio_t* hsend (hloop_t* loop, int connfd, const void* buf, size_t len, hwrite_cb write_cb) {
    hio_t* io = hio_get(loop, connfd);
    if (io == NULL) return NULL;
    io->send = 1;
    io->io_type = HIO_TYPE_TCP;
    return hwrite(loop, connfd, buf, len, write_cb);
}

hio_t* hrecvfrom (hloop_t* loop, int sockfd, void* buf, size_t len, hread_cb read_cb) {
    hio_t* io = hio_get(loop, sockfd);
    if (io == NULL) return NULL;
    io->recvfrom = 1;
    io->io_type = HIO_TYPE_UDP;
    return hread(loop, sockfd, buf, len, read_cb);
}

hio_t* hsendto (hloop_t* loop, int sockfd, const void* buf, size_t len, hwrite_cb write_cb) {
    hio_t* io = hio_get(loop, sockfd);
    if (io == NULL) return NULL;
    io->sendto = 1;
    io->io_type = HIO_TYPE_UDP;
    return hwrite(loop, sockfd, buf, len, write_cb);
}

// @server: socket -> bind -> hrecvfrom
hio_t* create_udp_server(hloop_t* loop, int port) {
    int bindfd = Bind(port, SOCK_DGRAM);
    if (bindfd < 0) {
        return NULL;
    }
    return hio_get(loop, bindfd);
}

// @client: Resolver -> socket -> hio_get -> hio_setpeeraddr
hio_t* create_udp_client(hloop_t* loop, const char* host, int port) {
    // IPv4
    struct sockaddr_in peeraddr;
    socklen_t addrlen = sizeof(peeraddr);
    memset(&peeraddr, 0, addrlen);
    peeraddr.sin_family = AF_INET;
    int ret = Resolver(host, (struct sockaddr*)&peeraddr);
    if (ret != 0) return NULL;
    peeraddr.sin_port = htons(port);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return NULL;
    }

    hio_t* io = hio_get(loop, sockfd);
    if (io == NULL) return NULL;
    hio_setpeeraddr(io, (struct sockaddr*)&peeraddr, addrlen);
    return io;
}
