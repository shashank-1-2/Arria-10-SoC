// BARC CODE
#include<stdio.h>
#include<stdlib.h>
#include<fcntl.h>
#include<stdint.h>
#include<unistd.h>
#include<sys/mman.h>
#include<sys/select.h>

#define UIO_DEVICE "/dev/uio0"
#define FPGA_REG_SIZE 0x1000

int main(){
    int fd;
    uint32_t irq_count;
    volatile uint32_t*fpga_regs;
    fd = open(UIO_DEVICE,0_RDWR);
    if(fd<0){
        perror("Cannot open UIO device");
        retunr -1;
    }

    fpga_regs = (volatile uint32_t*) mmap(NULL, FPGA_REG_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    if(fpga_regs == MAP_FAILED){
        perror("mmap failed");
        retunr -1;
    }

    printf("Interrupt handler running... \n");

    while(1){
        if(read(fd, &irq_count, sizeof(irq_count)) < 0){
            perror("read failed");
            break;
        }
    }

    printf("Interrupt received : %u\n" , irq_count);

    static unit32_t led_pattern = 0xE;
    fpga_regs[0] = led_pattern;
    led_pattern = ((led_pattern<<1) & 0xF | (led_pattern>>3) & 0x1);
    
    if(write(fd, &irq_count, sizeof(irq_count))<0){
        perror("write failed");
        break;
    }
    close(fd);
    return 0;
}








//DEEPSEEK UPDATED
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <errno.h>

#define UIO_DEVICE "/dev/uio0"
#define FPGA_REG_SIZE 0x1000

int main() {
    int fd;
    uint32_t irq_count = 0;
    volatile uint32_t *fpga_regs;
    
    // 1. Open UIO device
    fd = open(UIO_DEVICE, O_RDWR);  // ← Fixed: O_RDWR (letter O)
    if (fd < 0) {
        perror("Cannot open UIO device");
        return -1;  // ← Fixed: return
    }

    // 2. Memory-map FPGA registers
    fpga_regs = (volatile uint32_t *)mmap(NULL, FPGA_REG_SIZE, 
                                          PROT_READ | PROT_WRITE, 
                                          MAP_SHARED, fd, 0);

    if (fpga_regs == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;  // ← Fixed: return
    }

    printf("Interrupt handler running... Press Ctrl+C to stop.\n");
    
    uint32_t led_pattern = 0xE;  // ← Fixed: uint32_t
    
    while (1) {
        // 3. Block until an interrupt occurs
        if (read(fd, &irq_count, sizeof(irq_count)) < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, retry
            }
            perror("read failed");
            break;
        }
        
        // 4. INTERRUPT RECEIVED! Process it here
        printf("Interrupt received! Count: %u\n", irq_count);
        
        // 5. Toggle LED pattern (active-low logic)
        fpga_regs[0] = led_pattern;  // Write to FPGA register
        led_pattern = ((led_pattern << 1) & 0xF) | ((led_pattern >> 3) & 0x1);
        
        // 6. Acknowledge interrupt to UIO driver (write back the count)
        if (write(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("write failed");
            break;
        }
        
        // 7. UDP/Ethernet Notification (YOU WILL ADD THIS)
        // send_udp_message(irq_count);
    }
    
    // 8. Cleanup
    munmap((void *)fpga_regs, FPGA_REG_SIZE);
    close(fd);
    printf("Interrupt handler stopped.\n");
    
    return 0;
}












// DEEPSEEK -> CLAUDE UPDATE
/* ============================================================
 * PC SIDE: TCP listener for SoC interrupt handshake
 * ============================================================
 * Flow:
 *   1. Listens on PORT for SoC to connect
 *   2. On receiving "IRQ_HANDLED,count=<N>" from SoC
 *   3. Sends back "ACK,count=<N>" to complete the handshake
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT      5000
#define BUF_SIZE  64

int main(void) {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    printf("Listening for SoC on port %d...\n", PORT);

    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept failed");
        close(server_fd);
        return -1;
    }

    printf("SoC connected from %s\n", inet_ntoa(client_addr.sin_addr));

    char buf[BUF_SIZE];
    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t recvd = recv(client_fd, buf, sizeof(buf) - 1, 0);

        if (recvd <= 0) {
            printf("SoC disconnected\n");
            break;
        }

        buf[recvd] = '\0';
        printf("Received: %s", buf);

        uint32_t count = 0;
        if (sscanf(buf, "IRQ_HANDLED,count=%u", &count) == 1) {
            char ack[BUF_SIZE];
            int ack_len = snprintf(ack, sizeof(ack), "ACK,count=%u\n", count);
            send(client_fd, ack, ack_len, 0);
            printf("Sent: %s", ack);
        }
    }

    close(client_fd);
    close(server_fd);
    return 0;
}











// CLAUDE -> DEEPSEEK UPDATE
// Version 2.0: Enhanced PC Listener

/* ============================================================
 * PC SIDE: Enhanced TCP listener for SoC interrupt handshake
 * ============================================================
 * Improvements:
 *   - Multiple connection support via fork()
 *   - recv() timeout
 *   - Better error handling
 *   - Connection monitoring
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT      5000
#define BUF_SIZE  64
#define TIMEOUT_SEC 5

void handle_client(int client_fd, struct sockaddr_in client_addr) {
    char buf[BUF_SIZE];
    char client_ip[INET_ADDRSTRLEN];
    
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("[Client %s] Connected\n", client_ip);
    
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t recvd = recv(client_fd, buf, sizeof(buf) - 1, 0);
        
        if (recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - connection still alive
                continue;
            }
            perror("recv failed");
            break;
        } else if (recvd == 0) {
            printf("[Client %s] Disconnected\n", client_ip);
            break;
        }
        
        buf[recvd] = '\0';
        printf("[Client %s] Received: %s", client_ip, buf);
        
        uint32_t count = 0;
        if (sscanf(buf, "IRQ_HANDLED,count=%u", &count) == 1) {
            char ack[BUF_SIZE];
            int ack_len = snprintf(ack, sizeof(ack), "ACK,count=%u\n", count);
            send(client_fd, ack, ack_len, 0);
            printf("[Client %s] Sent: %s", client_ip, ack);
        } else {
            // Invalid message format
            char error_msg[] = "ERROR,invalid_format\n";
            send(client_fd, error_msg, strlen(error_msg), 0);
            printf("[Client %s] Invalid format received\n", client_ip);
        }
    }
    
    close(client_fd);
    printf("[Client %s] Connection closed\n", client_ip);
}

int main(void) {
    int server_fd;
    struct sockaddr_in server_addr;
    
    // Ignore SIGCHLD to prevent zombie processes
    signal(SIGCHLD, SIG_IGN);
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }
    
    if (listen(server_fd, 5) < 0) {  // ← Increased backlog
        perror("listen failed");
        close(server_fd);
        return -1;
    }
    
    printf("========================================\n");
    printf("  SoC Interrupt Handshake Server\n");
    printf("  Listening on port %d\n", PORT);
    printf("  Ready for SoC connections...\n");
    printf("========================================\n");
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, retry
            }
            perror("accept failed");
            continue;
        }
        
        // Fork to handle client in child process
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            close(server_fd);  // Child doesn't need listening socket
            handle_client(client_fd, client_addr);
            exit(0);
        } else if (pid > 0) {
            // Parent process
            close(client_fd);  // Parent doesn't need client socket
        } else {
            perror("fork failed");
            close(client_fd);
        }
    }
    
    close(server_fd);
    return 0;
}


// Integration with Your SoC Code
// Your SoC-side code (the interrupt handler) should now send messages like this:

// Inside your SoC interrupt handler
void send_interrupt_notification(uint32_t irq_count) {
    int sockfd;
    struct sockaddr_in server_addr;
    char msg[64];
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);  // PC port
    inet_pton(AF_INET, "192.168.0.100", &server_addr.sin_addr);  // PC IP
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return;  // PC not listening
    }
    
    // Send message
    snprintf(msg, sizeof(msg), "IRQ_HANDLED,count=%u\n", irq_count);
    send(sockfd, msg, strlen(msg), 0);
    
    // Wait for ACK
    char ack[64];
    recv(sockfd, ack, sizeof(ack) - 1, 0);
    printf("PC replied: %s", ack);
    
    close(sockfd);
}










//DEEPSEEK -> CLAUDE UPDATED
/* ============================================================
 * SoC SIDE: Interrupt Service Routine + Ethernet Handshake
 * ============================================================
 * Flow:
 *   1. FPGA interrupt fires -> UIO read() unblocks (this is our ISR trigger)
 *   2. irq_count is read from UIO
 *   3. A message "IRQ_HANDLED,count=<N>" is sent to PC over TCP
 *   4. SoC waits for PC to send back "ACK,count=<N>"
 *   5. Only after ACK is received -> LED pattern toggles
 *      (LED = visual confirmation that PC acknowledged the interrupt)
 *   6. write(fd) re-arms the interrupt at the UIO driver level
 *   7. Loop back and block on next interrupt
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define UIO_DEVICE     "/dev/uio0"
#define FPGA_REG_SIZE  0x1000

#define PC_IP_ADDR     "192.168.1.10"   /* <-- set to your PC's IP */
#define PC_PORT        5000             /* <-- must match PC listener port */
#define BOARD_ID       "SOC_01"         /* <-- give each board a unique ID */

#define MSG_BUF_SIZE   96
#define ACK_TIMEOUT_SEC 2                /* how long to wait for PC's ACK */

/* ---- Connects (or reconnects) to PC. Returns valid socket fd or -1. ---- */
int connect_to_pc(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket() failed");
        return -1;
    }

    struct sockaddr_in pc_addr;
    memset(&pc_addr, 0, sizeof(pc_addr));
    pc_addr.sin_family = AF_INET;
    pc_addr.sin_port   = htons(PC_PORT);

    if (inet_pton(AF_INET, PC_IP_ADDR, &pc_addr.sin_addr) <= 0) {
        perror("inet_pton failed");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&pc_addr, sizeof(pc_addr)) < 0) {
        perror("connect to PC failed");
        close(sock);
        return -1;
    }

    /* Set a receive timeout so we never block forever waiting for an ACK */
    struct timeval tv;
    tv.tv_sec  = ACK_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Connected to PC at %s:%d\n", PC_IP_ADDR, PC_PORT);
    return sock;
}

int main(void) {
    int fd;
    int pc_sock;
    uint32_t irq_count;
    volatile uint32_t *fpga_regs;
    static uint32_t led_pattern = 0xE;

    /* ---- Open UIO device handle ---- */
    fd = open(UIO_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open UIO device");
        return -1;
    }

    /* ---- Map FPGA register space (used to drive LEDs) ---- */
    fpga_regs = (volatile uint32_t *) mmap(NULL, FPGA_REG_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
    if (fpga_regs == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;
    }

    /* ---- Connect to PC once at startup ---- */
    pc_sock = connect_to_pc();
    if (pc_sock < 0) {
        fprintf(stderr, "Initial connection to PC failed, will retry on first interrupt\n");
    }

    printf("Interrupt handler running...\n");

    /* ================= Main ISR loop ================= */
    while (1) {

        /* Blocks until FPGA interrupt fires; UIO returns the count */
        if (read(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("read failed");
            break;
        }

        printf("Interrupt received: count = %u\n", irq_count);

        /* ---- Build "interrupt handled" message ---- */
        char msg[MSG_BUF_SIZE];
        int msg_len = snprintf(msg, sizeof(msg), "IRQ_HANDLED,board=%s,count=%u\n", BOARD_ID, irq_count);

        int handshake_ok = 0;

        /* Reconnect if we don't currently have a live socket */
        if (pc_sock < 0) {
            pc_sock = connect_to_pc();
        }

        if (pc_sock >= 0) {
            ssize_t sent = send(pc_sock, msg, msg_len, 0);

            if (sent < 0) {
                perror("send failed, attempting reconnect");
                close(pc_sock);
                pc_sock = connect_to_pc();
                if (pc_sock >= 0) {
                    sent = send(pc_sock, msg, msg_len, 0);
                }
            }

            if (sent >= 0) {
                printf("Sent to PC: %s", msg);

                /* ---- Wait for PC's ACK (completes the handshake) ---- */
                char ack_buf[MSG_BUF_SIZE];
                memset(ack_buf, 0, sizeof(ack_buf));
                ssize_t recvd = recv(pc_sock, ack_buf, sizeof(ack_buf) - 1, 0);

                if (recvd > 0) {
                    ack_buf[recvd] = '\0';
                    printf("Received from PC: %s\n", ack_buf);

                    /* Basic check: does ACK reference our board + count? */
                    char acked_board[32];
                    uint32_t acked_count = 0;
                    if (sscanf(ack_buf, "ACK,board=%31[^,],count=%u", acked_board, &acked_count) == 2
                        && acked_count == irq_count
                        && strcmp(acked_board, BOARD_ID) == 0) {
                        handshake_ok = 1;
                    } else {
                        fprintf(stderr, "ACK did not match expected board/count\n");
                    }
                } else if (recvd == 0) {
                    fprintf(stderr, "PC closed connection\n");
                    close(pc_sock);
                    pc_sock = -1;
                } else {
                    perror("recv (waiting for ACK) failed or timed out");
                }
            }
        }

        /* ---- LED feedback: only toggles once handshake is confirmed ---- */
        if (handshake_ok) {
            printf("Handshake complete for interrupt %u\n", irq_count);
            fpga_regs[0] = led_pattern;
            led_pattern = ((led_pattern << 1) & 0xF) | ((led_pattern >> 3) & 0x1);
        } else {
            fprintf(stderr, "Handshake failed for interrupt %u, LED not updated\n", irq_count);
        }

        /* ---- Re-arm interrupt at UIO level for next event ---- */
        if (write(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("write (re-arm) failed");
            break;
        }
    }

    if (pc_sock >= 0) close(pc_sock);
    munmap((void*)fpga_regs, FPGA_REG_SIZE);
    close(fd);
    return 0;
}



/* ============================================================
 * PC SIDE: Multi-SoC TCP listener for interrupt handshake
 * ============================================================
 * - fork()-per-connection: each SoC gets its own child process,
 *   so a slow/stuck board can never block another board's ACKs.
 * - Board identity included in the protocol so concurrent SoCs
 *   with overlapping irq_count values can still be told apart
 *   in logs.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT        5000
#define BUF_SIZE    96
#define TIMEOUT_SEC 5
#define MAX_BOARD_ID_LEN 32

void handle_client(int client_fd, struct sockaddr_in client_addr) {
    char buf[BUF_SIZE];
    char client_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("[%s] Connected\n", client_ip);

    struct timeval tv;
    tv.tv_sec  = TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t recvd = recv(client_fd, buf, sizeof(buf) - 1, 0);

        if (recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  /* idle timeout, board still connected, keep waiting */
            }
            perror("recv failed");
            break;
        } else if (recvd == 0) {
            printf("[%s] Disconnected\n", client_ip);
            break;
        }

        buf[recvd] = '\0';
        printf("[%s] Received: %s", client_ip, buf);

        char board_id[MAX_BOARD_ID_LEN];
        uint32_t count = 0;

        /* Protocol: "IRQ_HANDLED,board=<id>,count=<N>" */
        if (sscanf(buf, "IRQ_HANDLED,board=%31[^,],count=%u", board_id, &count) == 2) {
            char ack[BUF_SIZE];
            int ack_len = snprintf(ack, sizeof(ack), "ACK,board=%s,count=%u\n", board_id, count);
            send(client_fd, ack, ack_len, 0);
            printf("[%s] Sent: %s", client_ip, ack);
        } else {
            const char *error_msg = "ERROR,invalid_format\n";
            send(client_fd, error_msg, strlen(error_msg), 0);
            printf("[%s] Invalid format received\n", client_ip);
        }
    }

    close(client_fd);
    printf("[%s] Connection closed\n", client_ip);
}

int main(void) {
    int server_fd;
    struct sockaddr_in server_addr;

    signal(SIGCHLD, SIG_IGN);  /* auto-reap finished children, no zombies */

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 8) < 0) {   /* a few more boards' worth of backlog */
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    printf("========================================\n");
    printf("  Multi-SoC Interrupt Handshake Server\n");
    printf("  Listening on port %d\n", PORT);
    printf("========================================\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);                       /* child doesn't listen */
            handle_client(client_fd, client_addr);
            exit(0);
        } else if (pid > 0) {
            close(client_fd);                        /* parent doesn't serve */
        } else {
            perror("fork failed");
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}