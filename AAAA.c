/* ============================================================
 * SoC SIDE — Complete IRQ Handler with LED Blink + TCP Handshake
 * ============================================================
 * Platform : Cyclone V SoC (DE-series or equivalent)
 *            ARM DS-5 Eclipse / arm-linux-gnueabihf-gcc
 *
 * Address map (pb_lwh2f LW bridge @ 0xFF200000):
 *   sys_id         0x00000000
 *   led_pio        0x00000010   <- 4 LEDs on bits[3:0]
 *   button_pio     0x00000020
 *   dipsw_pio      0x00000030
 *   pio_ctrl_reg   0x00000040
 *   pio_status_reg 0x00000060
 *   ILC            0x00000100
 *   pcie_0         0x00010000
 *   irq_pio        0x00040000   <- interrupt source PIO
 *
 * PIO register layout (Intel/Altera standard, at each PIO base):
 *   +0x00 DATA         write=drive output, read=input state
 *   +0x04 DIRECTION    1=output, 0=input
 *   +0x08 INTERRUPTMASK 1=interrupt enabled for that bit
 *   +0x0C EDGECAPTURE  write 1 to clear; must clear in ISR or IRQ won't re-fire
 *
 * Flow per interrupt:
 *   1. read(uio_fd) unblocks  (HW IRQ fired, kernel UIO woke us)
 *   2. Read irq_count from UIO
 *   3. Read EDGECAPTURE to confirm which bit fired, then clear it
 *   4. Blink all 4 LEDs: ON 75ms -> OFF  (immediate visual feedback)
 *   5. Send "IRQ_HANDLED,board=<ID>,count=<N>" to PC over TCP
 *   6. Wait for "ACK,board=<ID>,count=<N>" from PC (2s timeout)
 *   7. Log handshake result to console
 *   8. write(uio_fd) re-arms the UIO interrupt for next event
 *   9. Loop
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

/* =========================================================
 * Hardware constants — derived from your address map
 * ========================================================= */

/* LW HPS-to-FPGA bridge physical base (Cyclone V standard) */
#define LW_BRIDGE_BASE      0xFF200000UL

/* Total span we need to mmap: up to irq_pio end = 0x40010
   Round up to next 4K page boundary = 0x41000             */
#define LW_BRIDGE_SPAN      0x41000

/* --- LED PIO (base 0x10 from bridge) --- */
#define LED_PIO_OFFSET      0x00000010
#define LED_DATA_REG        (LED_PIO_OFFSET + 0x00)   /* write to drive LEDs */

#define LED_ALL_ON          0x0F    /* bits[3:0] = 4 LEDs all ON  */
#define LED_ALL_OFF         0x00    /* all LEDs off               */

/* --- IRQ PIO (base 0x40000 from bridge) --- */
#define IRQ_PIO_OFFSET      0x00040000
#define IRQ_DATA_REG        (IRQ_PIO_OFFSET + 0x00)
#define IRQ_INTMASK_REG     (IRQ_PIO_OFFSET + 0x08)   /* set bit to enable IRQ */
#define IRQ_EDGECAP_REG     (IRQ_PIO_OFFSET + 0x0C)   /* write 1 to clear edge  */

/* --- pio_status_reg (base 0x60) --- */
#define STATUS_REG_OFFSET   0x00000060
#define STATUS_DATA_REG     (STATUS_REG_OFFSET + 0x00)

/* --- pio_ctrl_reg (base 0x40) --- */
#define CTRL_REG_OFFSET     0x00000040
#define CTRL_DATA_REG       (CTRL_REG_OFFSET + 0x00)

/* =========================================================
 * Network configuration — edit before flashing each board
 * ========================================================= */
#define PC_IP_ADDR          "192.168.1.10"  /* your PC's IP on the same subnet */
#define PC_PORT             5000
#define BOARD_ID            "SOC_01"        /* UNIQUE per board: SOC_01, SOC_02 ... */

/* =========================================================
 * Tunables
 * ========================================================= */
#define UIO_DEVICE          "/dev/uio0"
#define LED_BLINK_MS        75              /* blink duration in milliseconds */
#define ACK_TIMEOUT_SEC     2
#define MSG_BUF_SIZE        96

/* =========================================================
 * Helpers — register read/write through the mmap'd window
 * ========================================================= */
static inline void reg_write(volatile uint32_t *bridge, uint32_t offset_bytes, uint32_t val)
{
    /* offset is in bytes; bridge pointer is uint32_t* so divide by 4 for index */
    bridge[offset_bytes / 4] = val;
}

static inline uint32_t reg_read(volatile uint32_t *bridge, uint32_t offset_bytes)
{
    return bridge[offset_bytes / 4];
}

/* =========================================================
 * LED blink — all 4 LEDs ON for blink_ms, then OFF
 * ========================================================= */
static void blink_leds(volatile uint32_t *bridge, int blink_ms)
{
    reg_write(bridge, LED_DATA_REG, LED_ALL_ON);
    usleep(blink_ms * 1000);
    reg_write(bridge, LED_DATA_REG, LED_ALL_OFF);
}

/* =========================================================
 * TCP: connect to PC listener
 * Returns a valid socket fd, or -1 on failure.
 * Sets a receive timeout so recv() never blocks forever.
 * ========================================================= */
static int connect_to_pc(void)
{
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

    if (connect(sock, (struct sockaddr *)&pc_addr, sizeof(pc_addr)) < 0) {
        perror("connect to PC failed");
        close(sock);
        return -1;
    }

    /* Never block forever waiting for an ACK */
    struct timeval tv = { ACK_TIMEOUT_SEC, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("[NET] Connected to PC %s:%d as board %s\n", PC_IP_ADDR, PC_PORT, BOARD_ID);
    return sock;
}

/* =========================================================
 * TCP: send IRQ_HANDLED message and wait for ACK.
 * Returns 1 on successful handshake, 0 on any failure.
 * Will attempt one reconnect if the initial send fails.
 * pc_sock is updated in place if reconnect happens.
 * ========================================================= */
static int do_handshake(int *pc_sock, uint32_t irq_count)
{
    char msg[MSG_BUF_SIZE];
    int  msg_len = snprintf(msg, sizeof(msg),
                            "IRQ_HANDLED,board=%s,count=%u\n",
                            BOARD_ID, irq_count);

    /* Reconnect if we lost the socket at some point */
    if (*pc_sock < 0)
        *pc_sock = connect_to_pc();

    if (*pc_sock < 0) {
        fprintf(stderr, "[NET] No connection to PC, skipping handshake\n");
        return 0;
    }

    /* --- Send --- */
    ssize_t sent = send(*pc_sock, msg, msg_len, 0);
    if (sent < 0) {
        perror("[NET] send failed, attempting reconnect");
        close(*pc_sock);
        *pc_sock = connect_to_pc();
        if (*pc_sock < 0) return 0;
        sent = send(*pc_sock, msg, msg_len, 0);
        if (sent < 0) {
            perror("[NET] send failed after reconnect");
            return 0;
        }
    }
    printf("[NET] Sent    -> %s", msg);

    /* --- Wait for ACK --- */
    char ack_buf[MSG_BUF_SIZE];
    memset(ack_buf, 0, sizeof(ack_buf));
    ssize_t recvd = recv(*pc_sock, ack_buf, sizeof(ack_buf) - 1, 0);

    if (recvd <= 0) {
        if (recvd == 0)
            fprintf(stderr, "[NET] PC closed the connection\n");
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "[NET] ACK timed out after %ds\n", ACK_TIMEOUT_SEC);
        else
            perror("[NET] recv failed");
        close(*pc_sock);
        *pc_sock = -1;
        return 0;
    }

    ack_buf[recvd] = '\0';
    printf("[NET] Received <- %s", ack_buf);

    /* --- Validate ACK: must match our board + count --- */
    char     acked_board[32];
    uint32_t acked_count = 0;
    if (sscanf(ack_buf, "ACK,board=%31[^,],count=%u", acked_board, &acked_count) == 2
        && acked_count == irq_count
        && strcmp(acked_board, BOARD_ID) == 0)
    {
        return 1;  /* handshake complete */
    }

    fprintf(stderr, "[NET] ACK mismatch (got board=%s count=%u, expected board=%s count=%u)\n",
            acked_board, acked_count, BOARD_ID, irq_count);
    return 0;
}

/* =========================================================
 * main
 * ========================================================= */
int main(void)
{
    int              uio_fd;
    int              pc_sock;
    uint32_t         uio_irq_count;
    uint32_t         irq_total = 0;
    volatile uint32_t *bridge;

    printf("==============================================\n");
    printf("  SoC IRQ Handler — Board: %s\n", BOARD_ID);
    printf("  LED blink: %d ms per interrupt\n", LED_BLINK_MS);
    printf("  PC target: %s:%d\n", PC_IP_ADDR, PC_PORT);
    printf("==============================================\n\n");

    /* --------------------------------------------------
     * 1. Open UIO device (handle to FPGA interrupt)
     * -------------------------------------------------- */
    uio_fd = open(UIO_DEVICE, O_RDWR);
    if (uio_fd < 0) {
        perror("Cannot open " UIO_DEVICE);
        return -1;
    }

    /* --------------------------------------------------
     * 2. mmap the LW bridge so we can access all PIOs
     *    directly from userspace without /dev/mem tricks
     * -------------------------------------------------- */
    bridge = (volatile uint32_t *) mmap(NULL,
                                         LW_BRIDGE_SPAN,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED,
                                         uio_fd, 0);
    if (bridge == MAP_FAILED) {
        perror("mmap failed");
        close(uio_fd);
        return -1;
    }

    /* --------------------------------------------------
     * 3. Initialise hardware
     * -------------------------------------------------- */

    /* LEDs off — known state */
    reg_write(bridge, LED_DATA_REG, LED_ALL_OFF);
    printf("[HW] LEDs initialised to OFF\n");

    /* IRQ PIO: enable interrupt on bit 0, clear any stale edge capture */
    reg_write(bridge, IRQ_INTMASK_REG, 0x1);   /* unmask bit 0 */
    reg_write(bridge, IRQ_EDGECAP_REG, 0x1);   /* clear any pending edge */
    printf("[HW] IRQ PIO interrupt mask enabled, edge capture cleared\n");

    /* Snapshot status/ctrl registers for diagnostic */
    uint32_t status_val = reg_read(bridge, STATUS_DATA_REG);
    uint32_t ctrl_val   = reg_read(bridge, CTRL_DATA_REG);
    printf("[HW] pio_status_reg = 0x%08X\n", status_val);
    printf("[HW] pio_ctrl_reg   = 0x%08X\n", ctrl_val);

    /* --------------------------------------------------
     * 4. Connect to PC listener
     * -------------------------------------------------- */
    pc_sock = connect_to_pc();
    if (pc_sock < 0)
        fprintf(stderr, "[NET] Initial connect failed, will retry on first interrupt\n");

    printf("\n[MAIN] Waiting for interrupts...\n\n");

    /* ==================================================
     * 5. Main ISR loop — one iteration per interrupt
     * ================================================== */
    while (1) {

        /* --- Block here until FPGA IRQ fires ---
         * The UIO kernel driver handles the actual HW interrupt, clears it
         * at the GIC level, then wakes us here. It returns the cumulative
         * interrupt count into uio_irq_count.                            */
        if (read(uio_fd, &uio_irq_count, sizeof(uio_irq_count)) < 0) {
            perror("[UIO] read failed");
            break;
        }

        irq_total++;
        printf("--- Interrupt #%u (UIO count=%u) ---\n", irq_total, uio_irq_count);

        /* --- Clear the EDGECAPTURE register on irq_pio ---
         * CRITICAL: if you don't clear this, the PIO will keep asserting
         * the interrupt line and the next re-arm will fire immediately.  */
        uint32_t edge = reg_read(bridge, IRQ_EDGECAP_REG);
        printf("[HW] irq_pio EDGECAPTURE = 0x%X (clearing)\n", edge);
        reg_write(bridge, IRQ_EDGECAP_REG, edge);   /* write-1-to-clear */

        /* --- Blink all 4 LEDs for LED_BLINK_MS milliseconds ---
         * This is instant visual confirmation that the ISR has fired.   */
        printf("[LED] Blinking all 4 LEDs for %d ms\n", LED_BLINK_MS);
        blink_leds(bridge, LED_BLINK_MS);

        /* --- TCP handshake with PC --- */
        int ok = do_handshake(&pc_sock, irq_total);
        if (ok) {
            printf("[HANDSHAKE] Complete for interrupt #%u — PC acknowledged\n", irq_total);
        } else {
            fprintf(stderr, "[HANDSHAKE] FAILED for interrupt #%u\n", irq_total);
        }

        /* --- Re-arm UIO interrupt ---
         * Writing back to uio_fd tells the UIO kernel driver to re-enable
         * the interrupt at the GIC level, so the next HW IRQ wakes us.  */
        if (write(uio_fd, &uio_irq_count, sizeof(uio_irq_count)) < 0) {
            perror("[UIO] write (re-arm) failed");
            break;
        }

        printf("[UIO] Interrupt re-armed, waiting for next event\n\n");
    }

    /* Cleanup */
    reg_write(bridge, LED_DATA_REG, LED_ALL_OFF);   /* LEDs off on exit */
    if (pc_sock >= 0) close(pc_sock);
    munmap((void *)bridge, LW_BRIDGE_SPAN);
    close(uio_fd);
    return 0;
}









/* ============================================================
 * PC SIDE — Multi-SoC TCP Listener
 * ============================================================
 * Receives "IRQ_HANDLED,board=<ID>,count=<N>" from any SoC,
 * logs it, and sends back "ACK,board=<ID>,count=<N>".
 * fork()-per-connection so multiple SoC boards are handled
 * concurrently with zero cross-talk.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#define PORT            5000
#define BUF_SIZE        96
#define TIMEOUT_SEC     5
#define MAX_BOARD_ID    32

/* Print a timestamped log line */
static void logmsg(const char *board, const char *dir, const char *text)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d][board=%-8s]%s %s",
           t->tm_hour, t->tm_min, t->tm_sec, board, dir, text);
    fflush(stdout);
}

static void handle_client(int client_fd, struct sockaddr_in client_addr)
{
    char client_ip[INET_ADDRSTRLEN];
    char buf[BUF_SIZE];

    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

    /* Idle timeout on recv so a dead SoC doesn't park a process forever */
    struct timeval tv = { TIMEOUT_SEC, 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("[%s] SoC connected\n", client_ip);

    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t recvd = recv(client_fd, buf, sizeof(buf) - 1, 0);

        if (recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Idle timeout — SoC still alive, just quiet */
                continue;
            }
            perror("recv error");
            break;
        }
        if (recvd == 0) {
            printf("[%s] SoC disconnected cleanly\n", client_ip);
            break;
        }

        buf[recvd] = '\0';

        char     board_id[MAX_BOARD_ID];
        uint32_t count = 0;

        /* Expected format: "IRQ_HANDLED,board=<id>,count=<N>\n" */
        if (sscanf(buf, "IRQ_HANDLED,board=%31[^,],count=%u", board_id, &count) == 2) {

            logmsg(board_id, " -> RECV:", buf);

            /* Build and send the ACK */
            char ack[BUF_SIZE];
            int  ack_len = snprintf(ack, sizeof(ack),
                                    "ACK,board=%s,count=%u\n", board_id, count);
            send(client_fd, ack, ack_len, 0);

            logmsg(board_id, " <- SENT:", ack);

        } else {
            /* Malformed packet — tell the SoC explicitly */
            const char *err = "ERROR,invalid_format\n";
            send(client_fd, err, strlen(err), 0);
            fprintf(stderr, "[%s] Malformed packet: %s\n", client_ip, buf);
        }
    }

    close(client_fd);
    printf("[%s] Connection closed\n", client_ip);
}

int main(void)
{
    int server_fd;
    struct sockaddr_in server_addr;

    /* Auto-reap finished child processes (no zombie accumulation) */
    signal(SIGCHLD, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return -1;
    }
    if (listen(server_fd, 8) < 0) {
        perror("listen"); close(server_fd); return -1;
    }

    printf("==============================================\n");
    printf("  Multi-SoC Interrupt Handshake Server\n");
    printf("  Listening on port %d\n", PORT);
    printf("  Waiting for SoC boards to connect...\n");
    printf("==============================================\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* Child: handle this one SoC, parent's socket not needed */
            close(server_fd);
            handle_client(client_fd, client_addr);
            exit(0);
        } else if (pid > 0) {
            /* Parent: doesn't serve clients directly */
            close(client_fd);
        } else {
            perror("fork");
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
