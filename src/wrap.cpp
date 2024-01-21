#include "wrap.h"

void msg(const char *msg) { fprintf(stderr, "%s\n", msg); }

void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

int Socket(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd == -1) {
        die("socket()");
    }
    return fd;
}

int Bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int ret = bind(sockfd, addr, addrlen);
    if (ret == -1) {
        die("bind()");
    }
    return ret;
}

int Listen(int sockfd, int backlog) {
    int ret = listen(sockfd, backlog);
    if (ret == -1) {
        die("listen()");
    }
    return ret;
}

// 获取当前时间
uint64_t get_monotonic_usec() {
    timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_nsec / 1000);
}

uint32_t next_timer_ms() {

    uint64_t now_us = get_monotonic_usec();
    uint64_t next_us = (uint64_t)-1;

    // 空闲定时器
    if (dlist_empty(&g_data.idle_list)) {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        next_us = next->idle_start + k_idle_timeout_ms * 1000;
    }

    // ttl定时器
    if (!g_data.heap.empty() && g_data.heap[0].val < next_us) {
        next_us = g_data.heap[0].val;
    }

    if (next_us == (uint64_t)-1) {
        return 10000; // 没有定时器
    }

    if (next_us <= now_us) {
        return 0;
    }
    return (uint32_t)((next_us - now_us) / 1000);
}

void process_timers() {
    // 额外的1000us是poll()的响应时间
    uint64_t now_us = get_monotonic_usec() + 1000;

    // 空闲定时器
    while (!dlist_empty(&g_data.idle_list)) {
        Conn *next = container_of(g_data.idle_list.next, Conn, idle_list);
        uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
        if (next_us >= now_us + 1000) {
            break;
        }

        printf("removing idle connection: %d\n", next->fd);
        conn_done(next);
    }

    // ttl定时器
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    while (!g_data.heap.empty() && g_data.heap[0].val < now_us) {
        Entry *ent = container_of(g_data.heap[0].ref, Entry, heap_idx);
        HNode *node = hm_pop(&g_data.db, &ent->node, &hnode_same);
        assert(node == &ent->node);
        entry_del(ent);
        if (nworks++ >= k_max_works) {
            // 如果有太多的keys同时过期，不要让服务器停机
            break;
        }
    }
}

void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl errno");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, 0);
    if (errno) {
        die("fcntl errno");
    }
}

void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

int32_t accept_new_conn(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd == -1) {
        msg("accept() error");
        return -1;
    }

    fd_set_nb(connfd);

    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn->idle_start = get_monotonic_usec();
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);
    conn_put(g_data.fd2conn, conn);
    return 0;
}

void connection_io(Conn *conn) {
    /// 轮询唤醒，更新空闲定时器
    // 将conn移动到列表的末尾
    conn->idle_start = get_monotonic_usec();
    dlist_detach(&conn->idle_list);
    dlist_insert_before(&g_data.idle_list, &conn->idle_list);
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);
    }
}

void conn_done(Conn *conn) {
    g_data.fd2conn[conn->fd] = NULL;
    (void)close(conn->fd);
    dlist_detach(&conn->idle_list);
    free(conn);
}

void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {
    }
}

void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {
    }
}

bool try_fill_buffer(Conn *conn) {
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR); // EINTR:系统调用被信号中断
    if (rv < 0 && errno == EAGAIN) {    // EAGAIN:缓冲区已满
        return false;
    }
    if (rv < 0) {
        msg("read() error");
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // 尝试一个接一个地处理请求
    while (try_one_request(conn)) {
    }

    return (conn->state == STATE_REQ);
}

bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) { // 响应已全部发送，更改状态返回
        conn->state = STATE_REQ;
        conn->wbuf_size = 0;
        conn->wbuf_sent = 0;
        return false;
    }

    // wbuf中还有一些数据，尝试再次写入
    return true;
}

bool try_one_request(Conn *conn) {
    if (conn->rbuf_size < 4) { // 缓冲区中数据不足
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) { // 缓冲区中数据不足
        return false;
    }

    // 解析请求
    std::vector<std::string> cmd;
    if (parse_req(&conn->rbuf[4], len, cmd) != 0) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    // 得到一个请求，生成响应。
    std::string out;
    do_request(cmd, out);

    // 将响应打包到缓冲区中
    if (4 + out.size() > k_max_msg) {
        out.clear();
        out_err(out, ERR_2BIG, "response if too big");
    }
    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    // 从缓冲区移除请求
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);

    // 如果请求已经完全处理完毕，则继续外循环
    return (conn->state == STATE_REQ);
}
void do_request(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "pexpire")) {
        do_expire(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "pttl")) {
        do_ttl(cmd, out);
    } else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd")) {
        do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem")) {
        do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore")) {
        do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery")) {
        do_zquery(cmd, out);
    } else { // 无法识别命令
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

int32_t parse_req(const uint8_t *data, size_t len,
                  std::vector<std::string> &out) {
    if (len < 4) {
        return -1;
    }
    uint32_t n = 0;
    memcpy(&n, &data[0], 4);
    if (n > k_max_args) {
        return -1;
    }
    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) {
        return -1;
    }
    return 0;
}

bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}

void out_nil(std::string &out) { out.push_back(SER_NIL); }

void out_err(std::string &out, int32_t code, const std::string &msg) {
    out.push_back(SER_ERR);
    out.append((char *)&code, 4);
    uint32_t len = (uint32_t)msg.size();
    out.append((char *)&len, 4);
    out.append(msg);
}

void out_str(std::string &out, const std::string &val) {
    return out_str(out, val.data(), val.size());
}

void out_str(std::string &out, const char *s, size_t size) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)size;
    out.append((char *)&len, 4);
    out.append(s, len);
}

void out_int(std::string &out, int64_t val) {
    out.push_back(SER_INT);
    out.append((char *)&val, 8);
}

void out_dbl(std::string &out, double val) {
    out.push_back(SER_DBL);
    out.append((char *)&val, 8);
}

void out_arr(std::string &out, uint32_t n) {
    out.push_back(SER_ARR);
    out.append((char *)&n, 4);
}

void out_update_arr(std::string &out, uint32_t n) {
    assert(out[0] == SER_ARR);
    memcpy(&out[1], &n, 4);
}

void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg) {
    if (tab->size == 0) {
        return;
    }
    for (size_t i = 0; i < tab->mask + 1; i++) {
        HNode *node = tab->tab[i];
        while (node) {
            f(node, arg);
            node = node->next;
        }
    }
}

void cb_scan(HNode *node, void *arg) {
    std::string &out = *(std::string *)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

bool str2dbl(const std::string &s, double &out) {
    char *endp = NULL;
    out = strtod(s.c_str(), &endp); // 将字符串转换成浮点数
    return endp == s.c_str() + s.size() && !isnan(out);
}

bool str2int(const std::string &s, int64_t &out) {
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10); // 10是进制数
    return endp == s.c_str() + s.size();
}

// 在条目从键空间分离后销毁它
void entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);

    const size_t k_large_container_size = 10000;
    bool too_big = false;

    if (ent->type == T_ZSET) {
        too_big = hm_size(&ent->zset->hmap) > k_large_container_size;
    }

    if (too_big) {
        thread_pool_queue(&g_data.tp, &entry_del_async, ent);
    } else {
        entry_destroy(ent);
    }
}

void entry_del_async(void *arg) { entry_destroy((Entry *)arg); }

// 立即释放key
void entry_destroy(Entry *ent) {
    if (ent->type == T_ZSET) {
        zset_dispose(ent->zset);
        delete ent->zset;
    }
    delete ent;
}

bool expect_zset(std::string &out, std::string &s, Entry **ent) {
    Entry key;
    key.key.swap(s);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!hnode) {
        out_nil(out);
        return false;
    }

    *ent = container_of(hnode, Entry, node);
    if ((*ent)->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return false;
    }
    return true;
}

void do_keys(std::vector<std::string> &cmd, std::string &out) {
    // 防止未使用的参数导致的编译警告
    (void)cmd;
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    h_scan(&g_data.db.ht1, &cb_scan, &out);
    h_scan(&g_data.db.ht2, &cb_scan, &out);
}

void do_get(std::vector<std::string> &cmd, std::string &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->type != T_STR) {
        return out_err(out, ERR_TYPE, "expect string type");
    }
    out_str(out, ent->val);
}

void do_set(std::vector<std::string> &cmd, std::string &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        Entry *ent = container_of(node, Entry, node);
        if (ent->type != T_STR) {
            return out_err(out, ERR_TYPE, "expect string type");
        }
        ent->val.swap(cmd[2]);
    } else {
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    out_nil(out);
}

void do_del(std::vector<std::string> &cmd, std::string &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_pop(&g_data.db, &key.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? (int64_t)1 : (int64_t)0);
}

// cmd:zadd zset score name
void do_zadd(std::vector<std::string> &cmd, std::string &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expect fp number");
    }

    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);

    Entry *ent = NULL;
    if (!hnode) {
        ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        hm_insert(&g_data.db, &ent->node);
    } else {
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_TYPE, "expect zset");
        }
    }
    const std::string &name = cmd[3];
    bool added = zset_add(ent->zset, name.data(), name.size(), score);
    return out_int(out, (int64_t)added);
}

// cmd:zrem zset name
void do_zrem(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_pop(ent->zset, name.data(), name.size());
    if (znode) {
        znode_del(znode);
    }
    return out_int(out, znode ? (int64_t)1 : (int64_t)0);
}

// cmd:zscore zset name
void do_zscore(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(ent->zset, name.data(), name.size());
    return znode ? out_dbl(out, znode->score) : out_nil(out);
}

// cmd:zquery zset score name offset limit
void do_zquery(std::vector<std::string> &cmd, std::string &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expect fp number");
    }
    const std::string &name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset)) {
        return out_err(out, ERR_ARG, "expect int");
    }
    if (!str2int(cmd[5], limit)) {
        return out_err(out, ERR_ARG, "expect int");
    }

    Entry *ent = NULL;
    if (!expect_zset(out, cmd[1], &ent)) {
        if (out[0] == SER_NIL) {
            out.clear();
            out_arr(out, 0);
        }
        return;
    }
    if (limit <= 0) {
        return out_arr(out, 0);
    }
    ZNode *znode =
        zset_query(ent->zset, score, name.data(), name.size(), offset);

    out_arr(out, 0);
    uint32_t n = 0;
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        znode = container_of(avl_offset(&znode->tree, +1), ZNode, tree);
        n += 2;
    }
    return out_update_arr(out, n);
}

// cmd:pexpire ttl.name ttl.val(ms)
void do_expire(std::vector<std::string> &cmd, std::string &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) {
        return out_err(out, ERR_ARG, "expect int64");
    }

    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        Entry *ent = container_of(node, Entry, node);
        entry_set_ttl(ent, ttl_ms);
    }
    return out_int(out, node ? 1 : 0);
}

// pexpire ttl.name
void do_ttl(std::vector<std::string> &cmd, std::string &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        return out_int(out, -2);
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx == (size_t)-1) {
        return out_int(out, -1);
    }

    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now_us = get_monotonic_usec();
    return out_int(out, expire_at > now_us ? (expire_at - now_us) / 1000 : 0);
}

bool cmd_is(const std::string &word, const char *cmd) {
    return strcasecmp(word.c_str(), cmd) ==
           0; // strcasecmp函数比较大小时忽略大小写
}

bool hnode_same(HNode *lhs, HNode *rhs) { return lhs == rhs; }

// 设置或删除TTL
void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != (size_t)(-1)) {
        // 从堆中清除一个元素
        // 用数组中的最后一项替换它
        size_t pos = ent->heap_idx;
        g_data.heap[pos] = g_data.heap.back();
        g_data.heap.pop_back();
        if (pos < g_data.heap.size()) {
            heap_update(g_data.heap.data(), pos, g_data.heap.size());
        }
        ent->heap_idx = -1;
    } else if (ttl_ms >= 0) {
        size_t pos = ent->heap_idx;
        if (pos == (size_t)-1) {
            // 向堆中添加新项
            HeapItem item;
            item.ref = &ent->heap_idx;
            g_data.heap.push_back(item);
            pos = g_data.heap.size() - 1;
        }
        g_data.heap[pos].val = get_monotonic_usec() + (uint64_t)ttl_ms * 1000;
        heap_update(g_data.heap.data(), pos, g_data.heap.size());
    }
}
