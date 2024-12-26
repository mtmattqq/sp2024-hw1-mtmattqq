#include "server.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>

const int NSEC_PER_SEC = 1000000000;
const unsigned char IAC_IP[3] = "\xff\xf4";
const char* file_prefix = "./csie_trains/train_";
const char* status_prefix = "./trains_status/train_";
const char* accept_read_header = "ACCEPT_FROM_READ";
const char* accept_write_header = "ACCEPT_FROM_WRITE";
const char* welcome_banner = "======================================\n"
                             " Welcome to CSIE Train Booking System \n"
                             "======================================\n";

const char* lock_msg = ">>> Locked.\n";
const char* exit_msg = ">>> Client exit.\n";
const char* cancel_msg = ">>> You cancel the seat.\n";
const char* full_msg = ">>> The shift is fully booked.\n";
const char* seat_booked_msg = ">>> The seat is booked.\n";
const char* no_seat_msg = ">>> No seat to pay.\n";
const char* book_succ_msg = ">>> Your train booking is successful.\n";
const char* invalid_op_msg = ">>> Invalid operation.\n";

char* read_shift_msg = "Please select the shift you want to check [902001-902005]: ";
char* write_shift_msg = "Please select the shift you want to book [902001-902005]: ";
char* write_seat_msg = "Select the seat [1-40] or type \"pay\" to confirm: ";
char* write_seat_or_exit_msg = "Type \"seat\" to continue or \"exit\" to quit [seat/exit]: ";

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP);
// free resources used by a request instance

int accept_conn(void);
// accept connection

static void getfilepath(char* filepath, int extension);
// get record filepath

static void getstatuspath(char* filepath, int extension);
// get status filepath

int max(int a, int b) {
    return a > b ? a : b;
}

int lock_seat(int fd, int seat) {
    struct flock lock = write_lock;
    lock.l_start = seat * 2;
    lock.l_len = 1;
    return fcntl(fd, F_SETLKW, &lock);
}

int unlock_seat(int fd, int seat) {
    struct flock lock = unlock;
    lock.l_start = seat * 2;
    lock.l_len = 1;
    return fcntl(fd, F_SETLKW, &lock);
}

bool is_seat_locked(int fd, int seat) {
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = seat * 2,
        .l_len = 1
    };
    int ret = fcntl(fd, F_GETLK, &lock);
    if (ret < 0) ERR_EXIT("fcntl");
    return lock.l_type != F_UNLCK;
}

int handle_read(request* reqP) {
    /*  Return value:
     *      1: read successfully
     *      0: read EOF (client down)
     *     -1: read failed
         TODO: READ FASTER
     */
    int r, len;
    char cur = 0, buf[MAX_MSG_LEN];
    memset(buf, 0, sizeof(buf));
    // Read in request from client
    r = read(reqP->conn_fd, buf, MAX_MSG_LEN);
    if (r < 0) return -1;
    if (r == 0) return 0;
    char *p1 = strstr(buf, "\r\n"); // \r\n
    if (p1 == NULL) {
        p1 = strstr(buf, "\n");   // \n
    }
    if (p1 == NULL) {
        if (!strncmp(buf, (const char*) IAC_IP, 2)) {
            // Client presses ctrl+C, regard as disconnection
            fprintf(stderr, "Client presses ctrl+C....\n");
            return 0;
        }
        // Inconplete command
        len = strlen(buf);
    }
    else {
        len = p1 - buf;
        reqP->is_command_fully_read = true;
    }
    memcpy(reqP->buf + reqP->buf_len, buf, len);
    reqP->buf_len += len;
    reqP->buf[reqP->buf_len] = '\0';
    return 1;
}

train_info* get_train(int shift_id) {
    return trains + (shift_id - TRAIN_ID_START);
}

#ifdef READ_SERVER
int print_train_info(request *reqP) {
    char buf[MAX_MSG_LEN];
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < SEAT_NUM / 4; i++) {
        sprintf(buf + (i * 4 * 2), "%d %d %d %d\n", 
            get_train(reqP->booking_info.shift_id)->seat_stat[i * 4], 
            get_train(reqP->booking_info.shift_id)->seat_stat[i * 4 + 1], 
            get_train(reqP->booking_info.shift_id)->seat_stat[i * 4 + 2], 
            get_train(reqP->booking_info.shift_id)->seat_stat[i * 4 + 3]);
    }
    for (int i = 0; i < SEAT_NUM; ++i) {
        if (is_seat_locked(get_train(reqP->booking_info.shift_id)->file_fd, i)) {
            buf[i * 2] = '2';
        }
    }
    write(reqP->conn_fd, buf, strlen(buf));
    return 0;
}
#else
int print_train_info(request *reqP) {
    /*
     * Booking info
     * |- Shift ID: 902001
     * |- Chose seat(s): 1,2
     * |- Paid: 3,4
     */
    char buf[MAX_MSG_LEN * 2];
    char chosen_seat[MAX_MSG_LEN] = "";
    size_t chosen_len = 0;
    char paid[MAX_MSG_LEN] = "";
    size_t paid_len = 0;

    for (int i = 0; i < SEAT_NUM; ++i) {
        if (reqP->booking_info.seat_stat[i] == CHOSEN) {
            if (chosen_len == 0) {
                chosen_len += sprintf(chosen_seat + chosen_len, "%d", i + 1);
            }
            else {
                chosen_len += sprintf(chosen_seat + chosen_len, ",%d", i + 1);
            }
        }
        else if (reqP->booking_info.seat_stat[i] == PAID) {
            if (paid_len == 0) {
                paid_len += sprintf(paid + paid_len, "%d", i + 1);
            }
            else {
                paid_len += sprintf(paid + paid_len, ",%d", i + 1);
            }
        }
    }

    memset(buf, 0, sizeof(buf));
    sprintf(buf, "\nBooking info\n"
                 "|- Shift ID: %d\n"
                 "|- Chose seat(s): %s\n"
                 "|- Paid: %s\n\n",
                 reqP->booking_info.shift_id, chosen_seat, paid);
    write(reqP->conn_fd, buf, strlen(buf));
    return 0;
}
#endif

int write_train_info(int shift_id) {
    int i;
    char buf[MAX_MSG_LEN];
    memset(buf, 0, sizeof(buf));
    for (i = 0; i < SEAT_NUM / 4; i++) {
        sprintf(buf + (i * 4 * 2), "%d %d %d %d\n", 
            get_train(shift_id)->seat_stat[i * 4] == PAID ? 1 : 0, 
            get_train(shift_id)->seat_stat[i * 4 + 1] == PAID ? 1 : 0, 
            get_train(shift_id)->seat_stat[i * 4 + 2] == PAID ? 1 : 0, 
            get_train(shift_id)->seat_stat[i * 4 + 3] == PAID ? 1 : 0);
    }
    lseek(get_train(shift_id)->file_fd, 0, SEEK_SET);
    write(get_train(shift_id)->file_fd, buf, strlen(buf));
    return 0;
}

void try_accept_connection(struct pollfd *read_set, struct timespec current_time) {
    fd_set ready_set;
    FD_ZERO(&ready_set);
    FD_SET(svr.listen_fd, &ready_set);
    struct timeval ACCEPT_TIMEOUT = {0, 1 * 1000};  // 1 ms
    int ready_num = select(svr.listen_fd + 1, &ready_set, NULL, NULL, &ACCEPT_TIMEOUT);
    // Check new connection
    const int FD_RESERVED = 24;
    if (ready_num > 0 && alive_conn < MAX_CONNECTION - FD_RESERVED) {
        int connect_fd = accept_conn();
        if (connect_fd < 0) return;
        for (int i = 0; i < MAX_CONNECTION; i++) {
            if (read_set[i].fd == 0) {
                read_set[i].fd = connect_fd;
                read_set[i].events = POLLIN;
                break;
            }
        }
        requestP[connect_fd].status = SHIFT;
        requestP[connect_fd].start_time = current_time;
        write(connect_fd, welcome_banner, strlen(welcome_banner));
#ifdef READ_SERVER
        write(connect_fd, read_shift_msg, strlen(read_shift_msg));
#elif defined WRITE_SERVER
        write(connect_fd, write_shift_msg, strlen(write_shift_msg));
#endif
        alive_conn++;
    }
}

void free_pollfd(struct pollfd *read_element) {
#ifdef WRITE_SERVER
    int shift_id = requestP[read_element->fd].booking_info.shift_id;
    if (shift_id >= TRAIN_ID_START && shift_id <= TRAIN_ID_END) {
        for (int i = 0; i < SEAT_NUM; i++) {
            if (requestP[read_element->fd].booking_info.seat_stat[i] == CHOSEN) {
                get_train(shift_id)->seat_stat[i] = UNKNOWN;
                unlock_seat(get_train(shift_id)->file_fd, i);
            }
        }
    }
#endif
    printf("%s", exit_msg);
    close(read_element->fd);
    free_request(requestP + read_element->fd);
    read_element->fd = 0;
    read_element->events = 0;
    read_element->revents = 0;
    alive_conn--;
}

int select_shift(request* reqP) {
    int shift_id = atoi(reqP->buf);
    for (int i = 0; i < reqP->buf_len; i++) {
        if (!isdigit(reqP->buf[i])) {
            reqP->status = INVALID;
            return -1;
        }
    }
    if (shift_id < TRAIN_ID_START || shift_id > TRAIN_ID_END) {
        reqP->status = INVALID;
        return -1;
    }

#ifdef WRITE_SERVER
    bool fully_booked = true;
    for (int i = 0; i < SEAT_NUM; i++) {
        if (get_train(shift_id)->seat_stat[i] != PAID) {
            fully_booked = false;
            break;
        }
    }

    if (fully_booked) {
        write(reqP->conn_fd, full_msg, strlen(full_msg));
        return 0;
    }
#endif

    reqP->booking_info.shift_id = shift_id;
    reqP->booking_info.train_fd = trains[shift_id - TRAIN_ID_START].file_fd;
    reqP->status = SEAT;
    return 0;
}

int select_seat(request *reqP) {
    if (strncmp(reqP->buf, "pay", max(reqP->buf_len, 3)) == 0) {
        reqP->status = BOOKED;
        return 1;
    }
    for (int i = 0; i < reqP->buf_len; i++) {
        if (!isdigit(reqP->buf[i])) {
            reqP->status = INVALID;
            return -1;
        }
    }
    int seat_id = atoi(reqP->buf);
    if (seat_id < 1 || seat_id > 40) {
        reqP->status = INVALID;
        return -1;
    }
    // Check if the seat is already paid
    if (
        reqP->booking_info.seat_stat[seat_id - 1] == PAID ||
        get_train(reqP->booking_info.shift_id)->seat_stat[seat_id - 1] == PAID
    ) {
        write(reqP->conn_fd, seat_booked_msg, strlen(seat_booked_msg));
        return 0;
    }
    // Check if the seat is already chosen
    if (reqP->booking_info.seat_stat[seat_id - 1] == CHOSEN) {
        write(reqP->conn_fd, cancel_msg, strlen(cancel_msg));
        reqP->booking_info.seat_stat[seat_id - 1] = UNKNOWN;
        get_train(reqP->booking_info.shift_id)->seat_stat[seat_id - 1] = UNKNOWN;
        unlock_seat(reqP->booking_info.train_fd, seat_id - 1);
        reqP->booking_info.num_of_chosen_seats--;
        return 1;
    }
    // Check if the seat is locked by other client
    fprintf(stderr, "%d\n", get_train(reqP->booking_info.shift_id)->seat_stat[seat_id - 1]);
    if (
        is_seat_locked(get_train(reqP->booking_info.shift_id)->file_fd, seat_id - 1) ||
        get_train(reqP->booking_info.shift_id)->seat_stat[seat_id - 1] == CHOSEN
    ) {
        write(reqP->conn_fd, lock_msg, strlen(lock_msg));
        return 0;
    }
    else {
        reqP->booking_info.seat_stat[seat_id - 1] = CHOSEN;
        get_train(reqP->booking_info.shift_id)->seat_stat[seat_id - 1] = CHOSEN;
        lock_seat(reqP->booking_info.train_fd, seat_id - 1);
        reqP->booking_info.num_of_chosen_seats++;
        return 1;
    }
}

int pay(request *reqP) {
    if (reqP->booking_info.num_of_chosen_seats == 0) {
        write(reqP->conn_fd, no_seat_msg, strlen(no_seat_msg));
        reqP->status = SEAT;
        return 0;
    }
    for (int i = 0; i < SEAT_NUM; i++) {
        if (reqP->booking_info.seat_stat[i] == CHOSEN) {
            reqP->booking_info.seat_stat[i] = PAID;
            get_train(reqP->booking_info.shift_id)->seat_stat[i] = PAID;
            unlock_seat(reqP->booking_info.train_fd, i);
        }
    }
    write_train_info(reqP->booking_info.shift_id);
    write(reqP->conn_fd, book_succ_msg, strlen(book_succ_msg));
    reqP->booking_info.num_of_chosen_seats = 0;
    return 1;
}

int back_to_select_seat(request *reqP) {
    if (strncmp(reqP->buf, "seat", max(reqP->buf_len, 4)) == 0) {
        reqP->status = SEAT;
    }
    else {
        return -1;
    }
    return 0;
}

int read_handle_command(request* reqP) {
    if (strncmp(reqP->buf, "exit", max(reqP->buf_len, 4)) == 0) {
        write(reqP->conn_fd, exit_msg, strlen(exit_msg));
        return 0;
    }
    switch (reqP->status) {
    case SHIFT:
        select_shift(reqP);
        break;
    default:
        write(reqP->conn_fd, invalid_op_msg, strlen(invalid_op_msg));
        return -1;
    }
    switch (reqP->status) {
    case SEAT:
        reqP->status = SHIFT;
        print_train_info(reqP);
        break;
    default:
        write(reqP->conn_fd, invalid_op_msg, strlen(invalid_op_msg));
        return -1;
    }
    write(reqP->conn_fd, read_shift_msg, strlen(read_shift_msg));
    return 1;
}

int write_handle_command(request* reqP) {
    if (strncmp(reqP->buf, "exit", max(reqP->buf_len, 4)) == 0) {
        write(reqP->conn_fd, exit_msg, strlen(exit_msg));
        return 0;
    }
    int ret = 0;
    switch (reqP->status) {
    case SHIFT:
        ret = select_shift(reqP);
        break;
    case SEAT:
        ret = select_seat(reqP);
        break;
    case BOOKED:
        ret = back_to_select_seat(reqP);
        break;
    default:
        write(reqP->conn_fd, invalid_op_msg, strlen(invalid_op_msg));
        return -1;
    }
    if (ret < 0) reqP->status = INVALID;
    switch (reqP->status) {
    case SEAT:
        print_train_info(reqP);
        write(reqP->conn_fd, write_seat_msg, strlen(write_seat_msg));
        break;
    case BOOKED:
        if (!pay(reqP)) {
            print_train_info(reqP);
            write(reqP->conn_fd, write_seat_msg, strlen(write_seat_msg));
            break;
        }
        print_train_info(reqP);
        write(reqP->conn_fd, write_seat_or_exit_msg, strlen(write_seat_or_exit_msg));
        break;
    case INVALID:
        write(reqP->conn_fd, invalid_op_msg, strlen(invalid_op_msg));
        return -1;
    case SHIFT:
        write(reqP->conn_fd, write_shift_msg, strlen(write_shift_msg));
        break;
    }
    return 1;
}

void handle_request(struct pollfd *read_set, int ready_num) {
    char buf[MAX_MSG_LEN*2];
    int finished = 0;
    for (int i = 0; i < MAX_CONNECTION && finished < ready_num; ++i) {
        if (read_set[i].revents & POLLIN) {
            int conn_fd = read_set[i].fd;
            if (conn_fd == svr.listen_fd) {
                continue;
            }
            int ret = handle_read(&requestP[conn_fd]);
            if (ret < 0) write(conn_fd, invalid_op_msg, strlen(invalid_op_msg));
            if (ret == 0) write(conn_fd, exit_msg, strlen(exit_msg));
            if (ret <= 0) free_pollfd(read_set + i);

            // Handle requests from clients who are ready for read
            if (requestP[conn_fd].is_command_fully_read) {
                finished++;

                int ret = 0;
#ifdef READ_SERVER
                ret = read_handle_command(&requestP[conn_fd]);
#elif defined WRITE_SERVER
                ret = write_handle_command(&requestP[conn_fd]);    
#endif
                if (ret <= 0) free_pollfd(read_set + i);
                requestP[conn_fd].is_command_fully_read = false;
                requestP[conn_fd].buf_len = 0;
                memset(requestP[conn_fd].buf, 0, sizeof(requestP[conn_fd].buf));
            }
        }
    }
}

void check_connection_timeout(struct pollfd *read_set, struct timespec current_time) {
#ifndef DEBUG
    for (int i = 0; i < MAX_CONNECTION; i++) {
        if (read_set[i].fd == 0) continue;
        struct timespec diff_time = {
            .tv_sec = current_time.tv_sec - requestP[read_set[i].fd].start_time.tv_sec,
            .tv_nsec = current_time.tv_nsec - requestP[read_set[i].fd].start_time.tv_nsec
        };
        if (diff_time.tv_nsec < 0) {
            diff_time.tv_sec--;
            diff_time.tv_nsec += NSEC_PER_SEC;
        }
        if (diff_time.tv_sec >= 5 /*|| (diff_time.tv_nsec >= 950000000 && diff_time.tv_sec == 4)*/) {
            // write(read_set[i].fd, exit_msg, strlen(exit_msg));
            free_pollfd(read_set + i);
        }
    }
#endif
}

void parse_train_info(char *buf, int shift_id) {
    int cur = 0;
    for (int i = 0; i < SEAT_NUM; i++) {
        if (buf[cur] == '0' || buf[cur] == '1') {
            if (
                !is_seat_locked(get_train(shift_id)->file_fd, i) &&
                get_train(shift_id)->seat_stat[i] != CHOSEN
            )
                get_train(shift_id)->seat_stat[i] = buf[cur] - '0';
        }
        else {
            printf("Invalid seat status: %c\n", buf[cur]);
            exit(-1);
        }
        cur += 2;
    }
}

void read_train_info() {
    char buf[MAX_MSG_LEN];
    for (int i = TRAIN_ID_START, j = 0; i <= TRAIN_ID_END; i++, j++) {
        lseek(trains[j].file_fd, 0, SEEK_SET);
        read(trains[j].file_fd, buf, MAX_MSG_LEN);
        parse_train_info(buf, i);
    }
}

void open_files() {
    char filename[FILE_LEN];
    for (int i = TRAIN_ID_START, j = 0; i <= TRAIN_ID_END; i++, j++) {
        getfilepath(filename, i);
        if (trains[j].file_fd == 0) {
#ifdef READ_SERVER
            trains[j].file_fd = open(filename, O_RDONLY);
#elif defined WRITE_SERVER
            trains[j].file_fd = open(filename, O_RDWR);
#else
            trains[j].file_fd = -1;
#endif
        }
        // read train info from file
        if (trains[j].file_fd < 0) {
            ERR_EXIT("open");
        }
    }
    log_fd = open("log.txt", O_RDWR | O_CREAT | O_APPEND, 0644);
}

int main(int argc, char** argv) {

    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }

    // Initialize trains info and status
    open_files();
    read_train_info();

    // Initialize server
    init_server((unsigned short) atoi(argv[1]));

    // Loop for handling connections
    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n", svr.hostname, svr.port, svr.listen_fd, maxfd);

    // For checking connection timeout
    int last_read_time[MAX_CONNECTION] = {0};
    // For IO multiplexing (checking who are ready for read)
    struct pollfd read_set[MAX_CONNECTION] = {0};
    struct timespec current_time;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        // IO multiplexing
        try_accept_connection((struct pollfd*) &read_set, current_time); // 1ms
        int ready_num = poll((struct pollfd*) &read_set, MAX_CONNECTION, 9); // 9ms
        check_connection_timeout(
            read_set, 
            current_time
        );
        if (ready_num < 0) {
            if (errno == EINTR) continue;
            ERR_EXIT("poll");
        }
        if (ready_num == 0) continue;
        
        read_train_info();
        handle_request((struct pollfd*) &read_set,  ready_num);
    }

    free(requestP);
    close(svr.listen_fd);
    for (int i = 0;i < TRAIN_NUM; i++) {
        close(trains[i].file_fd);
    }

    return 0;
}

// ======================================================================================================
int accept_conn(void) {

    struct sockaddr_in cliaddr;
    size_t clilen;
    int conn_fd;  // fd for a new connection with client

    clilen = sizeof(cliaddr);
    conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliaddr, (socklen_t*)&clilen);
    if (conn_fd < 0) {
        if (errno == EINTR || errno == EAGAIN) return -1;  // try again
        if (errno == ENFILE) {
            (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
                return -1;
        }
        ERR_EXIT("accept");
    }
    
    requestP[conn_fd].conn_fd = conn_fd;
    strcpy(requestP[conn_fd].host, inet_ntoa(cliaddr.sin_addr));
    fprintf(stderr, "getting a new request... fd %d from %s\n", conn_fd, requestP[conn_fd].host);
    requestP[conn_fd].client_id = (svr.port * 1000) + num_conn;    // This should be unique for the same machine.
    num_conn++;
    
    return conn_fd;
}

static void getfilepath(char* filepath, int extension) {
    char fp[FILE_LEN*2];
    
    memset(filepath, 0, FILE_LEN);
    sprintf(fp, "%s%d", file_prefix, extension);
    strcpy(filepath, fp);
}

static void getstatuspath(char* filepath, int extension) {
    char fp[FILE_LEN*2];
    
    memset(filepath, 0, FILE_LEN);
    sprintf(fp, "%s%d", status_prefix, extension);
    strcpy(filepath, fp);
}

// ======================================================================================================
// You don't need to know how the following codes are working
#include <fcntl.h>

static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->client_id = -1;
    reqP->buf_len = 0;
    reqP->status = INVALID;
    reqP->start_time.tv_sec = 0;
    reqP->start_time.tv_nsec = 0;

    reqP->booking_info.num_of_chosen_seats = 0;
    reqP->booking_info.train_fd = -1;
    for (int i = 0; i < SEAT_NUM; i++)
        reqP->booking_info.seat_stat[i] = UNKNOWN;
}

static void free_request(request* reqP) {
    memset(reqP, 0, sizeof(request));
    init_request(reqP);
}

static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, MAX_CONNECTION) < 0) {
        ERR_EXIT("listen");
    }

    // Get file descripter table size and initialize request table
    maxfd = getdtablesize();
    requestP = (request*) malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (int i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    return;
}