#include "wrap.h"
#include <poll.h>

#define SERV_PORT 1234

Data g_data;

int main() {
    dlist_init(&g_data.idle_list);
    thread_pool_init(&g_data.tp, 4);

    int fd = Socket(AF_INET, SOCK_STREAM, 0);

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); // 端口复用

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;        // IPV4
    addr.sin_port = ntohs(SERV_PORT); // 转为网络字节序的端口号
    addr.sin_addr.s_addr = ntohl(INADDR_ANY); // 获取本机任意有效IP
    Bind(fd, (const sockaddr *)&addr,
         sizeof(addr)); // 给socket绑定地址结构(IP+port)

    Listen(fd, SOMAXCONN); // SOMAXCONN为监听上限

    fd_set_nb(fd); // 将监听fd设置为非阻塞模式

    // 事件循环
    std::vector<struct pollfd> poll_args;
    while (true) {
        poll_args.clear();

        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for (Conn *conn : g_data.fd2conn) {
            if (!conn) {
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events =
                (conn->state == STATE_REQ) ? POLLIN : POLLOUT; // 监听事件
            pfd.events |= POLLERR;
            poll_args.push_back(pfd);
        }
        int timeout_ms = (int)next_timer_ms();
        int ret = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (ret == -1) {
            die("poll");
        }
        // 处理主动连接
        for (size_t i = 1; i < poll_args.size(); i++) {
            if (poll_args[i].revents) { // 满足监听事件有返回值
                Conn *conn = g_data.fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) { // 删除该连接
                    conn_done(conn);
                }
            }
        }
        // 处理定时器
        process_timers();

        // 如果监听fd有返回值，获取该监听事件
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd);
        }
    }
    return 0;
}