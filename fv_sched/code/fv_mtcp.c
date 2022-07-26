#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>
#include <limits.h>

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include <numa.h>
#include <sys/stat.h>
#include "util.h"

#define True 1
#define False 0
#define QUEID 0
#define ETHER_OVR 24
#define IP_LEN 32
#define MAX_CPUS 16
#define POOL_SIZE 8192
#define BUF_SIZE 8192
#define MAX_CONN 1024
#define DEFAULT_MBUF_SIZE   (9618 + RTE_PKTMBUF_HEADROOM)
#define TIMEVAL_TO_MSEC(t)  ((t.tv_sec * 1000) + (t.tv_usec / 1000))
// vf ports: 0 1 2 3
// physical ports: 4 5 6 7
#define PRT_PF 4    /* the first physical port */
#define PAIR_NUM 4
#define CORE_NUM 8
#define NORMAL 5001

// 4 physical + 5 virtual
static int num_connection = 1;
// eth + ipv4 + tcp = 54
static int payload_len = 1464;
// zero for testing forever
static int exec_time = 0;
static int done[MAX_CPUS];
static in_addr_t daddr;
static in_addr_t saddr = INADDR_ANY;
static in_port_t dport_normal[PRT_PF];
pthread_cond_t cond[PAIR_NUM] = {PTHREAD_COND_INITIALIZER};
pthread_mutex_t lock[PAIR_NUM] = {PTHREAD_MUTEX_INITIALIZER};
static int sig_map[CORE_NUM] = {0, 1, 2, 3, 0, 1, 2, 3};
static int ready[PAIR_NUM] = {False};

struct thread_context
{
    int core;
    mctx_t mctx;
    int ep;
};
typedef struct thread_context* thread_context_t;

thread_context_t CreateContext(int core);
void CreateConnection(mctx_t mctx, int core, int ep, int dprt);
int ServicePortConfig(in_port_t dport, mctx_t mctx, int ep);
struct rte_mempool *mbuf_pool_create(const char *type, uint8_t pid, uint8_t queue_id,
			uint32_t nb_mbufs, int socket_id, int cache_size);
void RunSendContext(void *arg);
void RunRecvContext(void *arg);
void* RunVnf(void *arg);
void sigint_handler();

thread_context_t CreateContext(int core)
{
    thread_context_t ctx;
    ctx = (thread_context_t)malloc(sizeof(struct thread_context));
    if (!ctx) {
        perror("malloc");
        TRACE_CONFIG("Failed to allocate memory for thread context.\n");
        return NULL;
    }
    ctx->core = core;
    ctx->mctx = mtcp_create_context(core);
    if (!ctx->mctx) {
        TRACE_CONFIG("Failed to create mtcp context.\n");
        return NULL;
    }
    return ctx;
}

void CreateConnection(mctx_t mctx, int core, int ep, int dprt)
{
    int sockid, ret;
    struct mtcp_epoll_event ev;
    struct sockaddr_in dst_addr;
    // initiate new connection
    sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) {
        TRACE_CONFIG("Failed to create socket.\n");
        return;
    }
    ret = mtcp_setsock_nonblock(mctx, sockid);
    if (ret < 0) {
        TRACE_CONFIG("Failed to set socket in nonblocking mode.\n");
        exit(-1);
    }

    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(dprt);
    dst_addr.sin_addr.s_addr = daddr;

    ret = mtcp_connect(mctx, sockid, 
            (struct sockaddr *)&dst_addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        if (errno != EINPROGRESS) {
            TRACE_CONFIG("[CPU %d] Connection failed.\n", core);
            return;
        }
        // TRACE_CONFIG("ret: %d\n", ret);
    }

    TRACE_CONFIG("[CPU %d] Initiating stream %d\n", core, sockid);

    ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT | MTCP_EPOLLET;
    ev.data.sockid = sockid;
    ret = mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);
    if (ret < 0) {
        TRACE_CONFIG("[CPU %d] Failed to add epoll event for sockid: %d\n", core, sockid);
        exit(-1);
    }
}

int ServicePortConfig(in_port_t dport, mctx_t mctx, int ep) {

    int sockid, ret;
    struct mtcp_epoll_event ev;
    struct sockaddr_in addr;

    sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) {
		TRACE_CONFIG("Failed to create listening socket!\n");
		exit(-1);
	}

    ret = mtcp_setsock_nonblock(mctx, sockid);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		exit(-1);
	}
	
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = dport;
	
	ret = mtcp_bind(mctx, sockid, 
			(struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		exit(-1);
	}

	ret = mtcp_listen(mctx, sockid, 4096);
	if (ret < 0) {
		TRACE_ERROR("mtcp_listen() failed!\n");
		exit(-1);
	}

    ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT | MTCP_EPOLLET;
	ev.data.sockid = sockid;
	mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);

    return sockid;
}

struct rte_mempool *mbuf_pool_create(const char *type, uint8_t pid, uint8_t queue_id,
			uint32_t nb_mbufs, int socket_id, int cache_size) {
	struct rte_mempool *mp;
	char name[RTE_MEMZONE_NAMESIZE];

	snprintf(name, sizeof(name), "%-12s%u:%u", type, pid, queue_id);

	/* create the mbuf pool */
	mp = rte_pktmbuf_pool_create(name, nb_mbufs, cache_size,
		0, DEFAULT_MBUF_SIZE, socket_id);
	if (mp == NULL)
		TRACE_CONFIG(
			"Cannot create mbuf pool (%s) port %d, queue %d, nb_mbufs %d, socket_id %d: %s",
			name, pid, queue_id, nb_mbufs, socket_id, rte_strerror(rte_errno));

	return mp;
}

void RunSendContext(void *arg)
{
    // TRACE_CONFIG("This is sender.\n");
    int maxevents = 300000, nevents;
    int ep, core, ret;
	int i = 0;
    int complete_conn = 0;
    thread_context_t ctx;
    struct mtcp_epoll_event *events;
    mctx_t mctx;
    char buf[BUF_SIZE];
    struct timeval cur_tv, prev_tv;
    uint64_t diff_ts, snd_bytes = 0;

    ctx = (thread_context_t) arg;
    mctx = ctx->mctx;
    core = ctx->core;

    // build address pool
    // mtcp_init_rss(mctx, saddr, 1, daddr, dport_normal[core-PRT_PF]);
	mtcp_init_rss(mctx, saddr, 1, daddr, dport_normal[core]);

    // Initialization
    ep = mtcp_epoll_create(mctx, maxevents);
    if (ep < 0) {
        TRACE_CONFIG("Failed to create epoll struct.\n");
        exit(EXIT_FAILURE);
    }
    ctx->ep = ep;
	events = (struct mtcp_epoll_event *)
			calloc(maxevents, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_CONFIG("Failed to allocate events!\n");
		exit(EXIT_FAILURE);
	}

    // wait for receiver ready
    if (!ready[sig_map[core]]) {
        pthread_cond_wait(&cond[sig_map[core]], &lock[sig_map[core]]);
    }
	
	if (core == 3) {
		sleep(20);
	}

    // timer
    gettimeofday(&cur_tv, NULL);
    prev_tv = cur_tv;
	
    while (!done[core]) {

        while (complete_conn < num_connection) {
            // CreateConnection(mctx, core, ep, NORMAL+core-PRT_PF);
            CreateConnection(mctx, core, ep, NORMAL+core);
            complete_conn++;
        }

        nevents = mtcp_epoll_wait(mctx, ep, events, maxevents, -1);
        if (nevents > 0) {
            for (i = 0; i < nevents; i++) {
                if (events[i].events == MTCP_EPOLLOUT) {
                    // TRACE_CONFIG("[CPU %d] Established for stream %d\n", ctx->core, events[i].data.sockid);
                    while((ret = mtcp_write(mctx, events[i].data.sockid, buf, payload_len)) > 0) {
                        // usleep(1000);
                        snd_bytes += ret;
                    }
                    gettimeofday(&cur_tv, NULL);
                    diff_ts = TIMEVAL_TO_MSEC(cur_tv) - TIMEVAL_TO_MSEC(prev_tv);
                    if (diff_ts > 1000) {
                        // TRACE_CONFIG("[CPU %d] %f Gbps\n", ctx->core, snd_bytes*8.0/(diff_ts*1000000));
                        prev_tv = cur_tv;
                        snd_bytes = 0;
                    }
                }
                else if (events[i].events == MTCP_EPOLLIN) {
					while((ret = mtcp_read(mctx, events[i].data.sockid, buf, BUF_SIZE)) > 0);
                    // TRACE_CONFIG("[CPU %d] Stream %d send message %d.\n", ctx->core, events[i].data.sockid, num_send);
                }
                else if (events[i].events == MTCP_EPOLLERR) {
                    TRACE_CONFIG("[CPU %d] Stream %d: err.\n", ctx->core, events[i].data.sockid);
                    mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, 0);
                    mtcp_close(mctx, events[i].data.sockid);
					CreateConnection(mctx, core, ep, NORMAL+core-PRT_PF);
                }
                else {
                    TRACE_CONFIG("[CPU %d] Stream %d: event: %s.\n", ctx->core, events[i].data.sockid, EventToString(events[i].events));
                }
            }
        }
	}

    TRACE_CONFIG("Application thread %d out of loop.\n", core);
	mtcp_destroy_context(mctx);

	pthread_exit(NULL);

    return;
}

void RunRecvContext(void *arg)
{
    // TRACE_CONFIG("This is receiver.\n");
    int maxevents = 300000, nevents;
    int ep, core, sockid, ret, ev_sockid;
    int listen_normal;
    int i;
    thread_context_t ctx = NULL;
    struct mtcp_epoll_event *events = NULL;
    struct mtcp_epoll_event ev;
    mctx_t mctx = NULL;
    char buf[BUF_SIZE];
    struct timeval cur_tv, prev_tv;
    uint64_t diff_ts, recv_bytes = 0;

    ctx = (thread_context_t) arg;
    mctx = ctx->mctx;
    core = ctx->core;

	ep = mtcp_epoll_create(mctx, maxevents);
	if (ep < 0) {
		TRACE_ERROR("Failed to create epoll struct!\n");
		exit(EXIT_FAILURE);
	}
	ctx->ep = ep;
	events = (struct mtcp_epoll_event *)
			calloc(maxevents, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to allocate events!\n");
		exit(EXIT_FAILURE);
	}

    // config listening ports
    // listen_normal = ServicePortConfig(dport_normal[core], mctx, ep);
    listen_normal = ServicePortConfig(dport_normal[core-PRT_PF], mctx, ep);

    // signal sender
    ready[sig_map[core]] = True;
    pthread_cond_signal(&cond[sig_map[core]]);

    // timer
    gettimeofday(&cur_tv, NULL);
    prev_tv = cur_tv;

    while (!done[core]) {

		nevents = mtcp_epoll_wait(mctx, ep, events, maxevents, -1);
		// TRACE_CONFIG("[CPU %d] mtcp_epoll_wait returned %d events.\n", ctx->core, nevents);
		for (i = 0; i < nevents; i++) {
            
            if (events[i].events & MTCP_EPOLLERR) {
				TRACE_CONFIG("[CPU %d] Connection %d error.\n", ctx->core, events[i].data.sockid);
				mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_DEL, events[i].data.sockid, 0);
				mtcp_close(mctx, events[i].data.sockid);
				continue;
			}

			if (events[i].events == MTCP_EPOLLIN) {
                ev_sockid = events[i].data.sockid;
                if (ev_sockid == listen_normal) {
                    sockid = mtcp_accept(mctx, ev_sockid, NULL, NULL);
                    if (sockid >= 0) {
                        TRACE_CONFIG("[CPU %d] New connection %d accepted.\n", ctx->core, sockid);
                        ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT | MTCP_EPOLLET;
                        ev.data.sockid = sockid;
                        mtcp_epoll_ctl(mctx, ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);
                    }
					else {
						TRACE_CONFIG("[CPU %d] Accepted failded.\n", ctx->core);
					}
                }
				else {
                    while((ret = mtcp_read(mctx, ev_sockid, buf, BUF_SIZE)) > 0) {
                        recv_bytes += ret;
                    }
                    gettimeofday(&cur_tv, NULL);
                    diff_ts = TIMEVAL_TO_MSEC(cur_tv) - TIMEVAL_TO_MSEC(prev_tv);
                    if (diff_ts > 1000) {
                        // TRACE_CONFIG("[CPU %d] %f Gbps\n", ctx->core, recv_bytes*8.0/(diff_ts*1000000));
                        prev_tv = cur_tv;
                        recv_bytes = 0;
                    }
				}
			}
		}
	}

    TRACE_CONFIG("Application thread %d out of loop.\n", core);
	mtcp_destroy_context(mctx);

	TRACE_CONFIG("Application thread %d finished.\n", core);
	pthread_exit(NULL);
    return;
}

void* RunVnf(void *arg)
{
    int core;
    thread_context_t ctx;
    core = *(int *)arg;
    mtcp_core_affinitize(core);
    
    // initialization
    ctx = CreateContext(core);
    if (!ctx) return NULL;

#ifdef ENABLE_UCTX
    if (core < PRT_PF) { // vfs
        mtcp_create_app_context(ctx->mctx, (mtcp_app_func_t) RunSendContext, (void *) ctx);
        mtcp_run_app();
    }
    else { // pfs
        mtcp_create_app_context(ctx->mctx, (mtcp_app_func_t) RunRecvContext, (void *) ctx);
        mtcp_run_app();
    }
#endif

    // destroy mtcp context
    mtcp_destroy_context(ctx->mctx);
    pthread_exit(NULL);

    return NULL;
}

void sigint_handler()
{
    int i;
    for (i = 0; i < CORE_NUM; i++)
        done[i] = True;
}

int main(int argc, char **argv)
{
    int i, ret, cores[MAX_CPUS];
    pthread_t app_thread[MAX_CPUS];

    if (argc < 2) {
        TRACE_CONFIG("Please specify dst ip.");
        return False;
    }

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			num_connection = mystrtol(argv[i+1], 10);
		}
        else if (strcmp(argv[i], "-l") == 0) {
            payload_len = mystrtol(argv[i+1], 10);
        }
        else if (strcmp(argv[i], "-t") == 0) {
            exec_time = mystrtol(argv[i+1], 10);
        }
	}

    char dip_str[IP_LEN+1];
    strncpy(dip_str, argv[1], IP_LEN);
    daddr = inet_addr(dip_str);
    for (i = 0; i < PRT_PF; i++) {
        dport_normal[i] = htons(NORMAL+i);
    }
    srand(time(0));

    // set config parameters before calling mtcp_init func
    struct mtcp_conf mcfg;
    mtcp_getconf(&mcfg);
    mcfg.num_cores = CORE_NUM;
    mtcp_setconf(&mcfg);
    ret = mtcp_init("config/mtcp.conf");
    if (ret) {
        TRACE_CONFIG("Failed to initialize mtcp.\n");
        exit(EXIT_FAILURE);
    }
    mtcp_register_signal(SIGINT, sigint_handler);

    // start app thread
    for (i = 0; i < CORE_NUM; i++) {
        cores[i] = i;
        done[i] = False;
        if (pthread_create(&app_thread[i], NULL, RunVnf, (void *)&cores[i])) {
            perror("pthread_create");
            TRACE_CONFIG("Failed to create msg_test thread.\n");
            exit(EXIT_FAILURE);
        }
    }

    sleep(20);
    done[4] = True;
    // done[0] = True;
    sleep(20);
    done[6] = done[2] = True;
    sleep(20);
    done[7] = done[3] = True;
    sleep(20);

    // for (i = 1; i < PRT_PF; i++) {
        // done[i+PRT_PF] = True;
        // done[i] = True;
    // }

    // for (i = 0; i < CORE_NUM; i++) {
    //     pthread_join(app_thread[i], NULL);
    //     TRACE_CONFIG("Message test thread %d joined.\n", i);
    // }

    // mtcp_destroy();
    return 0;
}
