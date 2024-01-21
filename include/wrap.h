#ifndef WRAP_H
#define WRAP_H

#include "heap.h"
#include "list.h"
#include "thread_pool.h"
#include "zset.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

enum {
    STATE_REQ = 0, // 阅读请求
    STATE_RES = 1, // 发送响应
    STATE_END = 2,
};

enum {
    T_STR = 0,  // string
    T_ZSET = 1, // zset
};

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet *zset = NULL;
    size_t heap_idx = -1;
};

const size_t k_max_msg = 4096;
const size_t k_max_args = 1024;

struct Conn {
    int fd = -1;
    uint32_t state = 0;

    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];

    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];

    uint64_t idle_start = 0;
    DList idle_list;
};

struct Data {
    HMap db;
    // 所有客户端连接的映射
    std::vector<Conn *> fd2conn;
    // 用于空闲连接的定时器
    DList idle_list;
    // ttl的定时器
    std::vector<HeapItem> heap;
    // 线程池
    TheadPool tp;
};

extern Data g_data;

void msg(const char *msg);
void die(const char *msg);

int Socket(int domain, int type, int protocol);
int Bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int Listen(int sockfd, int backlog);

const uint64_t k_idle_timeout_ms = 5 * 1000;

uint64_t get_monotonic_usec();
uint32_t next_timer_ms();
void process_timers();

void fd_set_nb(int fd); // 将fd设置为非阻塞模式
void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn);
int32_t accept_new_conn(int fd); // 接受一个新的连接
void connection_io(Conn *conn);  // 客户端连接的状态机
void conn_done(Conn *conn);

void state_req(Conn *conn);
void state_res(Conn *conn);
bool try_fill_buffer(Conn *conn);
bool try_flush_buffer(Conn *conn);

bool try_one_request(Conn *conn); // 尝试从缓冲区解析请求
void do_request(std::vector<std::string> &cmd, std::string &out);
int32_t parse_req(const uint8_t *data, size_t len,
                  std::vector<std::string> &out); // 命令解析

bool entry_eq(HNode *lhs, HNode *rhs);

enum {
    ERR_UNKNOWN = 1, // 未知错误
    ERR_2BIG = 2,    // 太大
    ERR_TYPE = 3,    // 类型错误(string,zset)
    ERR_ARG = 4,     // 数据错误(int,float)
};

void out_nil(std::string &out);
void out_err(std::string &out, int32_t code, const std::string &msg);
void out_str(std::string &out, const char *val, size_t size);
void out_str(std::string &out, const std::string &val);
void out_int(std::string &out, int64_t val);
void out_dbl(std::string &out, double val);
void out_arr(std::string &out, uint32_t n);
void out_update_arr(std::string &out, uint32_t n);

void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg);
void cb_scan(HNode *node, void *arg);

bool str2dbl(const std::string &s, double &out);
bool str2int(const std::string &s, int64_t &out);

bool expect_zset(std::string &out, std::string &s, Entry **ent);
void entry_del(Entry *ent);
void entry_del_async(void *arg);
void entry_destroy(Entry *ent);

void do_keys(std::vector<std::string> &cmd, std::string &out);
void do_get(std::vector<std::string> &cmd, std::string &out);
void do_set(std::vector<std::string> &cmd, std::string &out);
void do_del(std::vector<std::string> &cmd, std::string &out);
void do_zadd(std::vector<std::string> &cmd, std::string &out);
void do_zrem(std::vector<std::string> &cmd, std::string &out);
void do_zscore(std::vector<std::string> &cmd, std::string &out);
void do_zquery(std::vector<std::string> &cmd, std::string &out);
void do_expire(std::vector<std::string> &cmd, std::string &out);
void do_ttl(std::vector<std::string> &cmd, std::string &out);

bool cmd_is(const std::string &word, const char *cmd);
bool hnode_same(HNode *lhs, HNode *rhs);

void entry_set_ttl(Entry *ent, int64_t ttl_ms);

#endif // WRAP_H