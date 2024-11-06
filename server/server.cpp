//     /============================================\
//     || ____          _ _       _          ____  ||
//     |||  _ \ ___  __| (_)___  (_)_ __    / ___| ||
//     ||| |_) / _ \/ _` | / __| | | '_ \  | |     ||
//     |||  _ <  __/ (_| | \__ \ | | | | | | |___  ||
//     |||_| \_\___|\__,_|_|___/ |_|_| |_|  \____| ||
//     \============================================/
//           Created by nektarios on 10/28/24.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <vector>
#include <map>
#include <iostream>

using std::cerr, std::endl;

const size_t k_max_msg = 4096; // Define your maximum message size
#define BUFFER_SIZE (4 + k_max_msg)  // Define your buffer size

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

// --------------------------- Start Up ------------------------------------

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,  // mark the connection for deletion
};

struct linearBuffer {
    uint8_t buffer[BUFFER_SIZE];
    size_t start;
    size_t end;
    size_t size;
};

// Initialize buffer
void init_buffer(linearBuffer *cb) {
    cb->start = 0;
    cb->end = 0;
    cb->size = 0;  // No data initially
}

struct Conn {
    int fd = -1;
    uint32_t state = 0;     // either STATE_REQ or STATE_RES
    // Buffer for reading
    linearBuffer rbuf;  // linear buffer for reading
    // Buffer for writing
    linearBuffer wbuf;  // linear buffer for writing
};

void init_conn(Conn *conn) {
    init_buffer(&conn->rbuf);
    init_buffer(&conn->wbuf);
}


static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;  // error
    }

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;

    init_buffer(&conn->rbuf);
    init_buffer(&conn->wbuf);
    conn_put(fd2conn, conn);
    return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

// --------------------------- GET SET DEL ------------------------------------

const size_t k_max_args = 1024;

static int32_t parse_req(
    const uint8_t *data, size_t len, std::vector<std::string> &out)
{
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
        return -1;  // trailing garbage
    }
    return 0;
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}

// The data structure for the key space. This is just a placeholder
// until we implement a hashtable in the next chapter.
static std::map<std::string, std::string> g_map;

static uint32_t do_get(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    std::string &val = g_map[cmd[1]];
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
}

static uint32_t do_del(
    const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map.erase(cmd[1]);
    return RES_OK;
}


static int32_t do_request(
    const uint8_t *req, uint32_t reqlen,
    uint32_t *rescode, uint8_t *res, uint32_t *reslen)
{
    std::vector<std::string> cmd;
    if (0 != parse_req(req, reqlen, cmd)) {
        msg("bad req");
        return -1;
    }
    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        // cmd is not recognized
        *rescode = RES_ERR;
        const char *msg = "Unknown cmd";
        strcpy((char *)res, msg);
        *reslen = strlen(msg);
        return 0;
    }
    return 0;
}

static bool try_one_request(Conn *conn) {
    cerr << "try_one_request\n";

    // Ensure there is enough data in the buffer (at least the size of the length field)
    if (conn->rbuf.size < 4) {
        cerr << "Not enough data in buffer. Buffer size: " << conn->rbuf.size << endl;
        return false; // Not enough data to process request
    }

    uint32_t len = 0;
    memcpy(&len, &conn->rbuf.buffer[0], 4);  // Copy 4 bytes to len
    // len = ntohl(len);  // Convert from network byte order to host byte order
    cerr << "Message length (len): " << len << endl;

    // If the message length is greater than the max size, reject it
    if (len > k_max_msg) {
        cerr << "Message too long. Max allowed: " << k_max_msg << ", received: " << len << endl;
        conn->state = STATE_END;
        return false;
    }

    // Ensure the buffer contains the full message (length + 4 bytes for the length field)
    if (conn->rbuf.size < len + 4) {
        cerr << "Not enough data in buffer to process full message. Buffer size: "
             << conn->rbuf.size << ", required: " << len + 4 << endl;
        return false; // Not enough data to process the full message
    }

    // got one request, generate the response.
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(
        &conn->rbuf.buffer[4], len,
        &rescode, &conn->wbuf.buffer[4+4], &wlen
    );
    if (err) {
        conn->state = STATE_END;
        return false;
    }

    wlen += 4;
    memcpy(&conn->wbuf.buffer[0], &wlen, 4);
    memcpy(&conn->wbuf.buffer[4], &rescode, 4);
    conn->wbuf.size = 4 + wlen;

    // Remove the request from the rbuf (linear buffer)
    size_t remain = conn->rbuf.size - (4 + len);
    cerr << "Remaining data in rbuf after message removal: " << remain << endl;

    if (remain > 0) {
        // Move the remaining data to the front of the buffer
        conn->rbuf.start += 4 + len;
        conn->rbuf.size = remain;
    } else {
        // Reset the buffer if no data is left
        conn->rbuf.start = 0;
        conn->rbuf.end = 0; // Ensure `start` and `end` are the same
        conn->rbuf.size = 0;
        cerr << "Buffer reset. rbuf.start: " << conn->rbuf.start << ", rbuf.size: " << conn->rbuf.size << ", rbuf.end: " << conn->rbuf.end << endl;
    }

    // Transition to the response state
    conn->state = STATE_RES;
    cerr << "Transitioning to response state. State: STATE_RES" << endl;
    state_res(conn); // Send the response

    // Continue processing if the request was fully handled
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    cerr << "try_fill_buffer\n";
    ssize_t rv = 0;

    // Calculate how much space is available in the buffer
    size_t available_space = BUFFER_SIZE - conn->rbuf.size;
    cerr << "available_space: " << available_space << endl;

    // If there is no space left, we stop reading data
    if (available_space == 0) {
        msg("Buffer is full, cannot read more data.");
        return false;
    }

    // Now calculate the space we can read into the buffer
    size_t cap = available_space;

    // Read data into the buffer
    do {
        rv = read(conn->fd, &conn->rbuf.buffer[conn->rbuf.end], cap);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        // Got EAGAIN, no data right now
        return false;
    }
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        msg("EOF reached");
        conn->state = STATE_END;
        return false;
    }

    // Update the end pointer and buffer size
    conn->rbuf.end += rv;
    conn->rbuf.size += rv;

    cerr << "rbuf.end: " << conn->rbuf.end << endl;
    cerr << "rbuf.size: " << conn->rbuf.size << endl;

    // Ensure the buffer size does not exceed the max size
    if (conn->rbuf.size > BUFFER_SIZE) {
        conn->rbuf.size = BUFFER_SIZE;  // Truncate to avoid overflow
    }

    // Process any received requests
    while (try_one_request(conn)) {}

    // Log the buffer state after processing
    cerr << "After processing: rbuf.size = " << conn->rbuf.size
         << ", rbuf.end = " << conn->rbuf.end << endl;

    return (conn->state == STATE_REQ);
}


static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;

    cerr << "try_flush_buffer\n";
    cerr << "Initial wbuf.size: " << conn->wbuf.size << endl;

    do {
        // Calculate how much data is left in the buffer to be written
        size_t remain = conn->wbuf.size; // Amount of data to write from wbuf
        cerr << "Attempting to write " << remain << " bytes from wbuf." << endl;

        rv = write(conn->fd, &conn->wbuf.buffer[conn->wbuf.start], remain);
        cerr << "write() returned: " << rv << " (errno: " << errno << ")" << endl;
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        // Got EAGAIN, stop
        cerr << "write() returned EAGAIN, will try again later." << endl;
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    // Update the start pointer and size of the buffer after writing
    conn->wbuf.start += rv;
    conn->wbuf.size -= rv;

    cerr << "After write, wbuf.size: " << conn->wbuf.size << ", wbuf.start: " << conn->wbuf.start << endl;

    // Check if the buffer is empty after writing
    if (conn->wbuf.size == 0) {
        // All data has been sent, reset buffer pointers
        cerr << "Buffer is empty after writing. Resetting buffer pointers." << endl;
        conn->wbuf.start = 0;
        conn->wbuf.end = 0;
        conn->state = STATE_REQ; // Go back to request state
        return false; // No more data to flush
    }

    // Still some data remains in buffer, could try to write again
    cerr << "Data remains in buffer, will attempt to write again." << endl;
    return true;
}


static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    printf("Server listening on: %d\n",ntohs(addr.sin_port));

    // a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // the event loop
    std::vector<struct pollfd> poll_args;
    while (true) {
        // prepare the arguments of the poll()
        poll_args.clear();
        // for convenience, the listening fd is put in the first position
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        // connection fds
        for (Conn *conn : fd2conn) {
            if (!conn) {
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        // poll for active fds
        // the timeout argument doesn't matter here
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) {
            die("poll");
        }

        // process active connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    fd2conn[conn->fd] = NULL;
                    (void)close(conn->fd);
                    free(conn);
                }
            }
        }

        // try to accept a new connection if the listening fd is active
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd2conn, fd);
        }
    }

    return 0;
}