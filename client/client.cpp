/* MIT License
 *
 * Copyright (c) 2019 Thomas Bruen
 * Copyright (c) 2019-2023 Kristian Sloth Lauszus
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <cerrno>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define BUF_SZ 100000

/* CONFIG PARAMETERS */
#ifndef DEBUG
#define DEBUG 0
#endif

#ifndef RECV_OWN_MSGS
#define RECV_OWN_MSGS 0 // If 1, we well receive messages we sent. useful for logging.
#endif

const int LOOPBACK = RECV_OWN_MSGS;

#ifndef WAIT_FOR_TCP_CONNECTION
#define WAIT_FOR_TCP_CONNECTION 0
#endif

#ifndef CLIENT_MODE
#define CLIENT_MODE 1
#endif

#ifndef CAN_FORWARDER_MODE
#define CAN_FORWARDER_MODE 0
#endif

// How long to wait for a new CAN-Bus frame while the TCP thread is busy, before checking if it is ready again
const int TCP_BUSY_POLL_TIMEOUT_MS = 10;

// The server sends frames as a uint32_t ID, a uint8_t DLC and 8 data bytes. A DLC larger than CAN_MAX_DLEN can not be a
// CAN frame, so these values are used for control messages from the server instead
// Sets the filters on the CAN socket. The ID is the number of CONTROL_FILTER messages that follow
const uint8_t CONTROL_SET_FILTERS = 0xF0;
// One filter: the ID is the filter ID and the first 4 data bytes are the filter mask in little-endian
const uint8_t CONTROL_FILTER = 0xF1;

// Older kernel headers do not define the maximum number of filters on a CAN socket
#ifndef CAN_RAW_FILTER_MAX
#define CAN_RAW_FILTER_MAX 512
#endif

/* DEFINITIONS */
#if !CAN_FORWARDER_MODE
// The timestamp is sent as 32-bit values, so the frames have the same size on 32 and 64-bit platforms, which is the
// size the TCPBus expects
typedef struct __attribute__((packed, aligned(1)))
{
    uint32_t tv_sec;
    uint32_t tv_usec;
    canid_t id;
    uint8_t dlc;
    uint8_t data[CAN_MAX_DLEN];
} timestamped_frame;
#endif

typedef struct
{
    int can_sock;
    bool use_unordered_map;
} can_read_args; // used to supply multiple thread args.

typedef struct
{
    int tcp_sock;
    int limit_recv_rate_hz;
} tcp_read_args; // used to supply multiple thread args.

typedef struct
{
    int tcp_sock;
    int can_sock;
} can_write_sockets; // used to supply multiple thread args.

/* FUNCTION DECLARATIONS */

// controlled exit in event of SIGINT
void handle_signal(int signal);

// print error information and exit (use before threads set-up)
void error(const char* msg, int error_code);

// print error information and exit thread
void pthread_error(const char* msg, int error_code);

// open up a TCP connection to the server
int create_tcp_socket(const char* hostname, int port);

// open a CAN socket
int open_can_socket(const char *port, const struct can_filter *filter, int numfilter, bool ignore_rx_error_frames);

// read a single CAN frame and add timestamp
#if CAN_FORWARDER_MODE
int read_frame(int soc, struct can_frame* frame, bool nonblock);
#else
int read_frame(int soc, struct can_frame* frame, bool nonblock, struct timeval* tv);
#endif

// continually read CAN and add to buffer
void* read_poll_can(void *args);

// continually dump read CAN buffer to TCP
void* read_poll_tcp(void *args);

// read from TCP and write to CAN socket
void* write_poll(void *args);

// convert bytes back to timestamped_can
#if CAN_FORWARDER_MODE
void deserialize_frame(const uint8_t* ptr, struct can_frame* frame);
#else
void deserialize_frame(const uint8_t* ptr, timestamped_frame* tf);
#endif

// print CAN frame into to stdout (for debug)
#if DEBUG
#if CAN_FORWARDER_MODE
void print_frame(const struct can_frame* frame);
#else
void print_frame(const timestamped_frame* tf);
#endif
#endif

/* GLOBALS */
pthread_mutex_t read_mutex = PTHREAD_MUTEX_INITIALIZER;
sig_atomic_t continue_running = true; // for "infinite loops" in threads.
bool tcp_ready_to_send = true; // only access inside of mutex
size_t socketcan_bytes_available = 0;// only access inside of mutex

pthread_cond_t tcp_send_copied; //signal to enable thread.

uint8_t read_buf_can[BUF_SZ]; // where serialized CAN frames are dumped
uint8_t read_buf_tcp[BUF_SZ]; // where serialized CAN frames are copied to and sent to the server

/* FUNCTIONS */
void handle_signal(int signal)
{
    (void)signal;
    continue_running = false;
}

void error(const char* msg, int error_code)
{
    continue_running = false;
    perror(msg);
    exit(error_code);
}

void pthread_error(const char* msg, int error_code)
{
    continue_running = false;
    fprintf(stderr, "%s: %d\n", msg, error_code);
    pthread_exit(&error_code);
}

int create_tcp_socket(const char* hostname, int port)
{
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct timeval tv;
    int client_fd;

    // Get the server address from the provided hostname
    server = gethostbyname(hostname);
    if (server == NULL)
    {
        error("ERROR, no such host", h_errno);
    }
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy(server->h_addr, &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(port);

#if CLIENT_MODE
    // If in client mode we create a socket and connect to the server
    client_fd = socket(AF_INET, SOCK_STREAM, 0); // TCP socket
    if (client_fd < 0)
    {
        error("ERROR opening socket", errno);
    }

    // Disable Nagle's algorithm, so the CAN messages are not grouped together into one TCP packet
    int flag = 1;
    int result = setsockopt(client_fd,    /* socket affected */
                            IPPROTO_TCP,  /* set option at TCP level */
                            TCP_NODELAY,  /* name of option */
                            &flag,        /* the cast is historical cruft */
                            sizeof(int)); /* length of option value */
    if (result < 0)
    {
        error("ERROR setting TCP_NODELAY", errno);
    }

    // Enable 1s timeout, so the thread is not blocking
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
    {
        error("ERROR setting timeout", errno);
    }

    if (connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
#if WAIT_FOR_TCP_CONNECTION
        // Close the socket, as a new one is created for every attempt
        close(client_fd);
        return -1;
#else
        error("ERROR connecting", errno);
#endif
    }
#else
    // If in server mode we bind to the provided hostname and port and wait for a client to connect
    struct sockaddr_in peer_addr;
    int server_fd;
    socklen_t peer_addr_size;

    server_fd = socket(AF_INET, SOCK_STREAM, 0); // TCP socket
    if (server_fd < 0)
    {
        error("ERROR opening socket", errno);
    }

    // Disable Nagle's algorithm, so the CAN messages are not grouped together into one TCP packet
    int flag = 1;
    int result = setsockopt(server_fd,    /* socket affected */
                            IPPROTO_TCP,  /* set option at TCP level */
                            TCP_NODELAY,  /* name of option */
                            &flag,        /* the cast is historical cruft */
                            sizeof(int)); /* length of option value */
    if (result < 0)
    {
        error("ERROR setting TCP_NODELAY", errno);
    }

    // Re-use the address to prevent "Address already in use" error
    const int reuseaddr = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0)
    {
        error("ERROR setting reuseaddr", errno);
    }

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        error("ERROR bind", errno);
    }

    if (listen(server_fd, 1) < 0)
    {
        error("ERROR listen", errno);
    }

    // Enable 1s timeout, so the accept is not blocking
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        error("ERROR setting timeout", errno);
    }

    // TODO: Remove this ugly hack
retry:
    if (!continue_running)
    {
        close(server_fd);
        return -1;
    }

    peer_addr_size = sizeof(peer_addr);
    client_fd = accept(server_fd, (struct sockaddr*)&peer_addr, &peer_addr_size);
    if (client_fd < 0)
    {
        goto retry;
        //error("ERROR accept", errno);
    }

    // Only one client is served, so stop listening
    close(server_fd);

    // Enable 1s timeout, so the thread is not blocking
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const uint8_t*)&tv, sizeof tv) < 0)
    {
        error("ERROR setting client timeout", errno);
    }
#endif

    return client_fd;
}

int open_can_socket(const char *can_interface_name, const struct can_filter *p_filter, int numfilter, bool ignore_rx_error_frames)
{
    struct ifreq ifr;
    struct sockaddr_can addr;
    int soc;

    // open socket
    soc = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (soc < 0)
    {
        error("ERROR failed to open socket", errno);
    }

    // configure socket
    if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &LOOPBACK, sizeof(LOOPBACK)) < 0)
    {
        error("ERROR setting loopback", errno);
    }

    // Enable 1s timeout, so the thread is not blocking
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (const uint8_t*)&tv, sizeof(tv)) < 0)
    {
        error("ERROR setting timeout", errno);
    }

    if (!ignore_rx_error_frames)
    {
        const int ERR_MASK = CAN_ERR_MASK; // Enable error frames
        if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &ERR_MASK, sizeof(ERR_MASK)) < 0)
        {
            error("ERROR enabling error frames", errno);
        }
    }

    if (numfilter > 0)
    {
        // Set filter and mask
        if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_FILTER, p_filter, numfilter * sizeof(struct can_filter)) < 0)
        {
            error("ERROR setting user filter and masks", errno);
        }
    }
    else
    {
        // Receive everything
        struct can_filter filter = { .can_id = 0, .can_mask = 0 };
        if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(struct can_filter)) < 0)
        {
            error("ERROR setting filter and masks", errno);
        }
    }

    addr.can_family = AF_CAN;
    strcpy(ifr.ifr_name, can_interface_name);
    if (ioctl(soc, SIOCGIFINDEX, &ifr) < 0)
    {
        error("ERROR failed to set ioctl", errno);
    }

    addr.can_ifindex = ifr.ifr_ifindex; // why does this have to be after ioctl?
    if (bind(soc, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        error("ERROR failed to bind to socket", errno);
    }

    return soc;
}

#if CAN_FORWARDER_MODE
int read_frame(int soc, struct can_frame* frame, bool nonblock)
#else
int read_frame(int soc, struct can_frame* frame, bool nonblock, struct timeval* tv)
#endif
{
    int bytes;

    int options = nonblock ? MSG_DONTWAIT : 0;
    bytes = recv(soc, frame, sizeof(*frame), options);

#if !CAN_FORWARDER_MODE
    ioctl(soc, SIOCGSTAMP, tv);
#endif

    return bytes;
}

void* read_poll_can(void* args)
{
#if DEBUG
    printf("read_poll_can started\n");
#endif

    const can_read_args* read_args = (const can_read_args*)args;
    int fd = read_args->can_sock;
    bool use_unordered_map = read_args->use_unordered_map;

    struct can_frame frame;
#if CAN_FORWARDER_MODE
    std::unordered_map<canid_t, struct can_frame> hash_map;
    size_t hash_map_value_size = sizeof(struct can_frame);
    const size_t frame_sz = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(frame.data);
#else
    std::unordered_map<canid_t, timestamped_frame> hash_map;
    size_t hash_map_value_size = sizeof(timestamped_frame);
    timestamped_frame tf;
    const size_t frame_sz = sizeof(tf);
    struct timeval tv;
#endif

    size_t count = 0;
    uint8_t* bufpnt = read_buf_can;

    while (continue_running)
    {
        // Do not block if there is data waiting to be sent, so it is sent as soon as there is no more data to read
        const bool nonblock = count != 0;
        if (nonblock)
        {
            pthread_mutex_lock(&read_mutex);
            const bool ready = tcp_ready_to_send;
            pthread_mutex_unlock(&read_mutex);
            if (!ready)
            {
                // The TCP thread is busy fx sleeping to limit the receive rate, so wait for the next frame instead
                // of reading without blocking in a loop, but wake up regularly to check if it is ready again
                struct pollfd pfd = { fd, POLLIN, 0 };
                (void)poll(&pfd, 1, TCP_BUSY_POLL_TIMEOUT_MS);
            }
        }

#if CAN_FORWARDER_MODE
        int num_bytes_can = read_frame(fd, &frame, nonblock);
#else
        int num_bytes_can = read_frame(fd, &frame, nonblock, &tv);
#endif
        if (num_bytes_can < 0)
        {
            // Return value of -1 happens when there is a timeout, we simply keep looping, as we want do not want to block,
            // as then we would never be able to shut down the threads if there was no activity on the bus
            // Negative return value other than -1 should not occur according to documentation.
        }
        else if (num_bytes_can == 0)
        {
            // This will happen when we shut down the client, so report an success
            pthread_error("Socket closed at other end... exiting", 0);
        }
        else
        {
#if !CAN_FORWARDER_MODE
            tf.tv_sec = (uint32_t)tv.tv_sec;
            tf.tv_usec = (uint32_t)tv.tv_usec;
            tf.id = frame.can_id;
            tf.dlc = frame.can_dlc;
            memcpy(tf.data, frame.data, sizeof(frame.data));
#endif

#if DEBUG
            printf("read %d number of bytes\n", num_bytes_can);
#if CAN_FORWARDER_MODE
            print_frame(&frame);
#else
            print_frame(&tf);
#endif
#endif

            if (use_unordered_map)
            {
#if CAN_FORWARDER_MODE
                hash_map[frame.can_id] = frame;
#else
                hash_map[tf.id] = tf;
#endif
                count = 1;
            }
            else
            {
                if (count > (BUF_SZ - frame_sz))
                {
                    //full buffer, drop data and start over. TODO: ring buffer, print/debug
                    fprintf(stderr, "Intermediate TCP-CAN buffer overflow");
                    bufpnt = read_buf_can;
                    count = 0;
                }

#if CAN_FORWARDER_MODE
                memcpy(bufpnt, (uint8_t*)&frame.can_id, sizeof(uint32_t));
                bufpnt += sizeof(uint32_t);
                count += sizeof(uint32_t);

                memcpy(bufpnt, (uint8_t*)&frame.can_dlc,sizeof(uint8_t));
                bufpnt += sizeof(uint8_t);
                count += sizeof(uint8_t);

                memcpy(bufpnt, (uint8_t*)&frame.data, sizeof(frame.data));
                bufpnt += sizeof(frame.data);
                count += sizeof(frame.data);
#else
                memcpy(bufpnt, (uint8_t*)&tf, sizeof(tf));
                bufpnt += sizeof(tf);
                count += sizeof(tf);
#endif
            }

#if DEBUG
            printf("message read: %zu\n", count);
#endif
        }

        if (count > 0)
        {
            pthread_mutex_lock(&read_mutex);
            if (tcp_ready_to_send) // other thread has said it is able to write to TCP socket
            {
                if (use_unordered_map)
                {
                    socketcan_bytes_available = hash_map.size() * hash_map_value_size;
                    size_t i = 0;
                    for (const auto &n : hash_map)
                    {
                        memcpy(read_buf_tcp + i, (uint8_t*)&n.second, hash_map_value_size);
                        i += hash_map_value_size;
                    }

                    tcp_ready_to_send = false;
                    const int signal_rv = pthread_cond_signal(&tcp_send_copied);
                    if (signal_rv < 0)
                    {
                        pthread_mutex_unlock(&read_mutex);
                        pthread_error("could not signal to other thread", signal_rv);
                    }

#if DEBUG
                    printf("%zu bytes copied to TCP buffer\n", socketcan_bytes_available);
#endif
                    hash_map.clear();
                    count = 0;
                }
                else
                {
                    socketcan_bytes_available = count;
                    memcpy(read_buf_tcp, read_buf_can, count);

                    tcp_ready_to_send = false;
                    const int signal_rv = pthread_cond_signal(&tcp_send_copied);
                    if (signal_rv < 0)
                    {
                        pthread_mutex_unlock(&read_mutex);
                        pthread_error("could not signal to other thread", signal_rv);
                    }

#if DEBUG
                    printf("%zu bytes copied to TCP buffer.\n", count);
#endif
                    bufpnt = read_buf_can; //start filling up buffer again
                    count = 0;
                }
            }
            pthread_mutex_unlock(&read_mutex);
        }
    }

    // Make sure the 'read_poll_tcp' thread is unblocked
    // No need to lock the mutex, as we do not care about predictable scheduling behaviour, as we are about to exit
    (void)pthread_cond_signal(&tcp_send_copied);

#if DEBUG
    printf("read_poll_can ended\n");
#endif

    pthread_exit(NULL);
}

void* read_poll_tcp(void* args)
{
#if DEBUG
    printf("read_poll_tcp started\n");
#endif

    const tcp_read_args* read_args = (const tcp_read_args*)args;
    int tcp_socket = read_args->tcp_sock;
    int limit_recv_rate_hz = read_args->limit_recv_rate_hz;

    size_t cpy_socketcan_bytes_available;
    int wait_rv = 0;
    while (continue_running)
    {
        pthread_mutex_lock(&read_mutex);
        tcp_ready_to_send = true;
        while (!socketcan_bytes_available)
        {
            wait_rv = pthread_cond_wait(&tcp_send_copied, &read_mutex);
            if (!continue_running)
            {
                // Break out if the continue_running flag has gone low
                socketcan_bytes_available = 0; // We do not care about the data, as we are about to exit
                break;
            }
        }
        if (wait_rv < 0)
        {
            pthread_mutex_unlock(&read_mutex);
            pthread_error("could not resume TCP send thread", wait_rv);
        }
        cpy_socketcan_bytes_available = socketcan_bytes_available; // we should only access the original inside a mutex.
        socketcan_bytes_available = 0;
        pthread_mutex_unlock(&read_mutex);

        // don't want to perform the write inside mutex;
#if DEBUG
        printf("ready to send %zu bytes\n", cpy_socketcan_bytes_available);
#endif
        int n = write(tcp_socket, read_buf_tcp, cpy_socketcan_bytes_available);
        if (n < 0)
        {
            if (errno == ENOBUFS)
            {
                pthread_error("no buffer space available on TCP socket", errno);
            }
            else
            {
                pthread_error("failed to write bytes over TCP socket", errno);
            }
        }
        else if ((size_t)n < cpy_socketcan_bytes_available)
        {
            fprintf(stderr, "only send %d bytes of TCP message.\n", n);
            pthread_error("failed to sent all bytes over TCP socket", EXIT_FAILURE);
        }

#if DEBUG
        printf("%d bytes written to TCP\n",n);
#if CAN_FORWARDER_MODE
        struct can_frame frame;
#else
        timestamped_frame frame;
#endif
        deserialize_frame(read_buf_tcp, &frame); //TODO: more than one frame.
        print_frame(&frame);
#endif

        if (limit_recv_rate_hz > 0)
        {
            struct timespec ts;
            int milliseconds = (int)roundf(1000.0f / (float)limit_recv_rate_hz);
            ts.tv_sec = milliseconds / 1000;
            ts.tv_nsec = (milliseconds % 1000) * 1000000;
            nanosleep(&ts, NULL);
        }
    }

#if DEBUG
    printf("read_poll_tcp ended\n");
#endif

    pthread_exit(NULL);
}

void* write_poll(void* args)
{
#if DEBUG
    printf("write_poll started\n");
#endif

    // CAN write should be quick enough to do in this loop...
    const can_write_sockets* socks = (const can_write_sockets*)args;
    struct can_frame frame;

    uint8_t write_buf[BUF_SZ];
    const size_t can_struct_sz = sizeof(struct can_frame);
    const size_t frame_sz = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(frame.data);

    // A read from the TCP socket can end in the middle of a frame, so the remainder is kept until the next read
    size_t num_bytes_buffered = 0;

    // The filters are applied once all the filters sent by the server have been received
    std::vector<struct can_filter> filters;
    size_t num_filters_expected = 0;
    bool filters_rejected = false;

    while (continue_running)
    {
        int num_bytes_tcp = read(socks->tcp_sock, write_buf + num_bytes_buffered, BUF_SZ - num_bytes_buffered);
        if (num_bytes_tcp < 0)
        {
            // Return value of -1 happens when there is a timeout, we simply keep looping, as we want do not want to block,
            // as then we would never be able to shut down the threads if there was no new data over TCP
            // Negative return value other than -1 should not occur according to documentation.
            continue;
        }
        else if (num_bytes_tcp == 0)
        {
            // This will happen when we shut down the client, so report an success
            pthread_error("Socket closed at other end... exiting", 0);
        }
        num_bytes_buffered += num_bytes_tcp;

#if DEBUG
        printf("%d bytes read from TCP. Number of frames: %zu\n", num_bytes_tcp, num_bytes_buffered / frame_sz);
#endif

        const uint8_t* bufpnt = write_buf;
        for (; num_bytes_buffered >= frame_sz; num_bytes_buffered -= frame_sz, bufpnt += frame_sz)
        {
            const uint32_t can_id = ((uint32_t)bufpnt[0]) | ((uint32_t)bufpnt[1] << 8) | ((uint32_t)bufpnt[2] << 16) | ((uint32_t)bufpnt[3] << 24);
            const uint8_t dlc = bufpnt[4];

            if (dlc == CONTROL_SET_FILTERS)
            {
                filters.clear();
                filters_rejected = can_id > CAN_RAW_FILTER_MAX;
                if (filters_rejected)
                {
                    // The kernel would reject them, so ignore the filters that follow and keep the current ones
                    fprintf(stderr, "Ignoring %u CAN filters, as at most %d are supported\n", can_id, CAN_RAW_FILTER_MAX);
                    num_filters_expected = 0;
                    continue;
                }
                num_filters_expected = can_id;
                if (num_filters_expected == 0)
                {
                    // No filters means that no frames are received
                    if (setsockopt(socks->can_sock, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0) < 0)
                    {
                        perror("ERROR setting the CAN filters");
                    }
                }
                continue;
            }
            else if (dlc == CONTROL_FILTER)
            {
                if (filters.size() >= num_filters_expected)
                {
                    if (!filters_rejected)
                    {
                        fprintf(stderr, "Ignoring a CAN filter that was not announced\n");
                    }
                    continue;
                }

                struct can_filter filter;
                filter.can_id = can_id;
                filter.can_mask = ((uint32_t)bufpnt[5]) | ((uint32_t)bufpnt[6] << 8) | ((uint32_t)bufpnt[7] << 16) | ((uint32_t)bufpnt[8] << 24);
                filters.push_back(filter);
                if (filters.size() == num_filters_expected)
                {
                    // The kernel swaps the filters atomically, so this is safe while the other thread is reading
                    if (setsockopt(socks->can_sock, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                                   filters.size() * sizeof(struct can_filter)) < 0)
                    {
                        perror("ERROR setting the CAN filters");
                    }
#if DEBUG
                    printf("Set %zu CAN filters\n", filters.size());
#endif
                }
                continue;
            }
            else if (dlc > CAN_MAX_DLEN)
            {
                fprintf(stderr, "Ignoring a frame with an invalid DLC: %u\n", dlc);
                continue;
            }

            frame.can_id = can_id;
            frame.can_dlc = dlc;
            memcpy(frame.data, &bufpnt[5], frame.can_dlc);

#if DEBUG
#if CAN_FORWARDER_MODE
            print_frame(&frame);
#else
            printf("frame | ID: %x | DLC: %u | Data:", frame.can_id, frame.can_dlc);
            for (int m = 0; m < frame.can_dlc; m++)
            {
                printf(" %02x",frame.data[m]);
            }
            printf("\n");
#endif
#endif
            int num_bytes_can = write(socks->can_sock, &frame, can_struct_sz);
            if (num_bytes_can < 0)
            {
                if (errno == ENOBUFS)
                {
                    pthread_error("no buffer space available on CAN socket", errno);
                }
                else
                {
                    pthread_error("failed to write bytes over CAN socket", errno);
                }
            }
            else if ((size_t)num_bytes_can < can_struct_sz)
            {
                fprintf(stderr, "only send %d bytes of can message.\n", num_bytes_can);
                pthread_error("failed to send complete CAN message!", EXIT_FAILURE);
            }
        }

        // Move the start of an incomplete frame to the beginning of the buffer
        memmove(write_buf, bufpnt, num_bytes_buffered);
    }

#if DEBUG
    printf("write_poll ended\n");
#endif

    pthread_exit(NULL);
}

#if CAN_FORWARDER_MODE
void deserialize_frame(const uint8_t* ptr, struct can_frame* frame)
#else
void deserialize_frame(const uint8_t* ptr, timestamped_frame* tf)
#endif
{
    size_t count = 0;
#if CAN_FORWARDER_MODE
    memcpy(&frame->can_id, ptr + count, sizeof(canid_t));
    count+= sizeof(canid_t);

    memcpy(&frame->can_dlc, ptr + count, sizeof(uint8_t));
    count += sizeof(uint8_t);

    memcpy(frame->data, ptr + count, frame->can_dlc);
#else
    // tf = (timestamped_frame*)ptr; // doesn't work, struct does some padding. manually populate fields?
    memcpy(&tf->tv_sec, ptr, sizeof(tf->tv_sec));
    count += sizeof(tf->tv_sec);

    memcpy(&tf->tv_usec, ptr + count, sizeof(tf->tv_usec));
    count += sizeof(tf->tv_usec);

    memcpy(&tf->id, ptr + count, sizeof(canid_t));
    count+= sizeof(canid_t);

    memcpy(&tf->dlc, ptr + count, sizeof(uint8_t));
    count += sizeof(uint8_t);

    memcpy(tf->data, ptr + count, tf->dlc);
#endif
}

#if DEBUG
#if CAN_FORWARDER_MODE
void print_frame(const struct can_frame* frame)
#else
void print_frame(const timestamped_frame* tf)
#endif
{
#if CAN_FORWARDER_MODE
    bool is_extended = frame->can_id & CAN_EFF_FLAG;
    bool is_error_frame = frame->can_id & CAN_ERR_FLAG;
    bool is_remote_frame = frame->can_id & CAN_RTR_FLAG;

    uint32_t arbitration_id;
    if (is_extended)
    {
        arbitration_id = frame->can_id & CAN_EFF_MASK;
    }
    else
    {
        arbitration_id = frame->can_id & CAN_SFF_MASK;
    }

    if (is_error_frame)
    {
        printf("ERROR FRAME | ");
    }

    if (is_remote_frame)
    {
        printf("Remote frame | ");
    }

    const uint8_t max_id_length = is_extended ? 3 : 8;
    printf("ID: 0x%0*X | DLC: %u | Data:", max_id_length, arbitration_id, frame->can_dlc);
    for (int n = 0; n < frame->can_dlc; n++)
    {
        printf(" %02x", frame->data[n]);
    }
#else
    printf("\t%lld.%lld: ID: 0x%x | DLC: %u | Data:", static_cast<long long int>(tf->tv_sec), static_cast<long long int>(tf->tv_usec), tf->id, tf->dlc);
    for (int n=0; n<tf->dlc; n++)
    {
        printf(" %02x", tf->data[n]);
    }
#endif
    printf("\n");
}
#endif

int tcpclient(const char *can_interface_name, const char *hostname, int port, const struct can_filter *filter, int numfilter, bool use_unordered_map, int limit_recv_rate_hz, bool ignore_rx_error_frames)
{
#if DEBUG
    printf("tcpclient started\n");
#endif

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pthread_t read_can_thread, read_tcp_thread, write_thread;
    int tcp_socket, can_socket, thread_rv;

    // initialising stuff
    thread_rv = pthread_mutex_init(&read_mutex, NULL);
    if (thread_rv != 0)
    {
        error("mutex init has failed", thread_rv);
    }

    can_socket = open_can_socket(can_interface_name, filter, numfilter, ignore_rx_error_frames);
    if (can_socket < 0)
    {
        error("unable to create read can thread", can_socket);
    }

#if WAIT_FOR_TCP_CONNECTION
#if DEBUG
    printf("Waiting for TCP connection");
#endif
    do {
#if DEBUG
        printf(".");
        fflush(stdout);
#endif
        tcp_socket = create_tcp_socket(hostname, port);
        sleep(1);
    } while (continue_running && tcp_socket == -1);
#if DEBUG
    if (tcp_socket != -1)
    {
        printf("\nTCP connection established\n");
    }
    else
    {
        printf("\n");
    }
#endif
#else
    tcp_socket = create_tcp_socket(hostname, port);
#endif

    if (tcp_socket == -1)
    {
        // The client was stopped before the TCP connection was established
        close(can_socket);
        return 0;
    }

    can_read_args read_args_can = { can_socket, use_unordered_map };
    thread_rv = pthread_create(&read_can_thread, NULL, read_poll_can, (void*)&read_args_can);
    if (thread_rv < 0)
    {
        error("unable to create read can thread", thread_rv);
    }

    tcp_read_args read_args_tcp = { tcp_socket, limit_recv_rate_hz };
    thread_rv = pthread_create(&read_tcp_thread, NULL, read_poll_tcp, (void*)&read_args_tcp);
    if (thread_rv < 0)
    {
        error("unable to create read tcp thread", thread_rv);
    }

    can_write_sockets write_args = { tcp_socket, can_socket };
    thread_rv = pthread_create(&write_thread, NULL, write_poll, (void*)&write_args);
    if (thread_rv < 0)
    {
        error("unable to create write thread", thread_rv);
    }

    thread_rv = pthread_join(read_can_thread, NULL);
    if (thread_rv < 0)
    {
        error("read can thread failed", thread_rv);
    }

    thread_rv = pthread_join(read_tcp_thread, NULL);
    if (thread_rv < 0)
    {
        error("read tcp thread failed", thread_rv);
    }

    thread_rv = pthread_join(write_thread, NULL);
    if (thread_rv < 0)
    {
        error("write thread failed", thread_rv);
    }

    close(tcp_socket);
    close(can_socket);

#if DEBUG
    printf("tcpclient ended\n");
#endif

    return 0;
}

int main(int argc, char* argv[])
{
    // arg parsing
    if (argc < 4)
    {
        fprintf(stderr, "usage %s can-name hostname port\n", argv[0]);
        exit(0);
    }

    char *can_interface_name = argv[1];
    char *hostname = argv[2];
    int port = atoi(argv[3]);

    return tcpclient(can_interface_name, hostname, port, NULL, 0, false, -1, false);
}
