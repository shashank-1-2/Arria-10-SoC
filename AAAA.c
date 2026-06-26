//date: 26/06/26
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>

#define UIO_DEVICE          "/dev/uio0"
#define FPGA_REG_SIZE       0x1000

/* Platform Designer Addresses (from your list) */
#define LW_H2F_BASE         0xFF200000
#define PIO_STATUS_REG_BASE 0x00000060
#define PIO_CTRL_REG_BASE   0x00000040
#define LED_PIO_BASE        0x00000010
#define BUTTON_PIO_BASE     0x00000020
#define DIPSW_PIO_BASE      0x00000030
#define IRQ_PIO_BASE        0x00040000

/* Offsets for mmap */
#define PIO_STATUS_REG_OFFSET   (PIO_STATUS_REG_BASE - LW_H2F_BASE)
#define PIO_CTRL_REG_OFFSET     (PIO_CTRL_REG_BASE - LW_H2F_BASE)
#define LED_PIO_OFFSET          (LED_PIO_BASE - LW_H2F_BASE)
#define BUTTON_PIO_OFFSET       (BUTTON_PIO_BASE - LW_H2F_BASE)
#define DIPSW_PIO_OFFSET        (DIPSW_PIO_BASE - LW_H2F_BASE)
#define IRQ_PIO_OFFSET          (IRQ_PIO_BASE - LW_H2F_BASE)

/* Interrupt Message Constants */
#define IRQ_MSG_START           0xAA
#define IRQ_MSG_END             0x55
#define IRQ_MSG_IDLE            0x00

/* LED Pattern (Active Low: 0 = ON, 1 = OFF) */
#define LED_PATTERN_START       0xE     /* 1110 = LED0 ON */
#define LED_ALL_OFF             0xF
#define LED_ALL_ON              0x0

/* Interrupt Control Flags */
#define IRQ_ENABLE              1
#define IRQ_DISABLE             0

/* Timeouts */
#define HANDSHAKE_TIMEOUT_MS    100     /* 100ms timeout for handshake */
#define RETRY_COUNT             3       /* Number of retries */

/* ============================================================
 * Function: micro_delay
 * Simple delay for hardware timing
 * ============================================================ */
void micro_delay(useconds_t us) {
    usleep(us);
}

/* ============================================================
 * Function: verify_hardware_handshake
 * Writes to pio_ctrl_reg and verifies via pio_status_reg
 * Returns: 1 on success, 0 on failure
 * ============================================================ */
int verify_hardware_handshake(volatile uint32_t *pio_ctrl_reg, 
                               volatile uint32_t *pio_status_reg,
                               uint32_t message) {
    uint32_t read_back;
    int retry;
    
    /* Step 1: Write the message to control register */
    *pio_ctrl_reg = message;
    printf("    [WRITE] pio_ctrl_reg = 0x%02X\n", message);
    
    /* Small delay for hardware to propagate */
    micro_delay(100);
    
    /* Step 2: Read back from status register with retries */
    for (retry = 0; retry < RETRY_COUNT; retry++) {
        read_back = *pio_status_reg;
        printf("    [READ]  pio_status_reg = 0x%02X (attempt %d)\n", 
               read_back, retry + 1);
        
        if (read_back == message) {
            printf("    ✅ HANDSHAKE SUCCESS! Message matched.\n");
            return 1;
        }
        
        /* Wait before retry */
        micro_delay(1000);
    }
    
    /* Step 3: Handshake failed */
    printf("    ❌ HANDSHAKE FAILED! Expected 0x%02X, got 0x%02X\n", 
           message, read_back);
    return 0;
}

/* ============================================================
 * Function: disable_interrupts
 * Masks interrupts at the hardware level
 * ============================================================ */
void disable_interrupts(volatile uint32_t *irq_pio) {
    /* Write 0 to interrupt enable register (or set mask bit) */
    *irq_pio = IRQ_DISABLE;
    printf("    🔒 Interrupts DISABLED\n");
    micro_delay(100);
}

/* ============================================================
 * Function: enable_interrupts
 * Unmasks interrupts at the hardware level
 * ============================================================ */
void enable_interrupts(volatile uint32_t *irq_pio) {
    /* Write 1 to interrupt enable register (or clear mask bit) */
    *irq_pio = IRQ_ENABLE;
    printf("    🔓 Interrupts ENABLED\n");
    micro_delay(100);
}

/* ============================================================
 * Function: process_interrupt
 * Handles the interrupt: writes message, verifies, updates LED
 * Returns: 1 if handshake successful, 0 otherwise
 * ============================================================ */
int process_interrupt(uint32_t irq_count,
                      volatile uint32_t *pio_ctrl_reg,
                      volatile uint32_t *pio_status_reg,
                      volatile uint32_t *led_pio,
                      volatile uint32_t *irq_pio,
                      uint32_t *led_pattern) {
    
    int handshake_ok = 0;
    uint32_t message;
    
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  🔔 INTERRUPT RECEIVED! Count: %u\n", irq_count);
    printf("═══════════════════════════════════════════════════════════════\n");
    
    /* ---- Step 1: DISABLE interrupts while processing ---- */
    disable_interrupts(irq_pio);
    
    /* ---- Step 2: Write START marker to control register ---- */
    handshake_ok = verify_hardware_handshake(pio_ctrl_reg, 
                                              pio_status_reg, 
                                              IRQ_MSG_START);
    
    if (!handshake_ok) {
        printf("    ❌ START marker handshake failed!\n");
        goto handshake_failed;
    }
    
    /* ---- Step 3: Write interrupt count ---- */
    message = irq_count & 0xFF;
    handshake_ok = verify_hardware_handshake(pio_ctrl_reg, 
                                              pio_status_reg, 
                                              message);
    
    if (!handshake_ok) {
        printf("    ❌ COUNT marker handshake failed!\n");
        goto handshake_failed;
    }
    
    /* ---- Step 4: Write END marker ---- */
    handshake_ok = verify_hardware_handshake(pio_ctrl_reg, 
                                              pio_status_reg, 
                                              IRQ_MSG_END);
    
    if (!handshake_ok) {
        printf("    ❌ END marker handshake failed!\n");
        goto handshake_failed;
    }
    
    /* ---- Step 5: ALL HANDSHAKES SUCCESSFUL! ---- */
    printf("    ✅ ALL HANDSHAKES SUCCESSFUL!\n");
    
    /* Update LED pattern (visual confirmation) */
    *led_pio = *led_pattern;
    printf("    [WRITE] LED Pattern: 0x%02X\n", *led_pattern);
    
    /* Rotate pattern for next interrupt */
    *led_pattern = ((*led_pattern << 1) & 0xF) | ((*led_pattern >> 3) & 0x1);
    
    /* Re-arm UIO interrupt */
    printf("    ✅ Interrupt processed and serviced.\n");
    
handshake_failed:
    /* ---- Step 6: ENABLE interrupts for next event ---- */
    enable_interrupts(irq_pio);
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    return handshake_ok;
}

/* ============================================================
 * MAIN FUNCTION
 * ============================================================ */
int main() {
    int fd;
    uint32_t irq_count = 0;
    uint32_t led_pattern = LED_PATTERN_START;
    uint32_t handshake_failures = 0;
    uint32_t total_interrupts = 0;
    
    volatile uint32_t *fpga_regs;
    volatile uint32_t *led_pio;
    volatile uint32_t *pio_ctrl_reg;
    volatile uint32_t *pio_status_reg;
    volatile uint32_t *button_pio;
    volatile uint32_t *dipsw_pio;
    volatile uint32_t *irq_pio;
    
    /* ---- 1. Open UIO device ---- */
    fd = open(UIO_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open UIO device");
        return -1;
    }
    
    /* ---- 2. Memory-map FPGA register space ---- */
    fpga_regs = (volatile uint32_t *)mmap(NULL, FPGA_REG_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    
    if (fpga_regs == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;
    }
    
    /* ---- 3. Set up register pointers ---- */
    led_pio       = (volatile uint32_t *)((uintptr_t)fpga_regs + LED_PIO_OFFSET);
    pio_ctrl_reg  = (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_CTRL_REG_OFFSET);
    pio_status_reg= (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_STATUS_REG_OFFSET);
    button_pio    = (volatile uint32_t *)((uintptr_t)fpga_regs + BUTTON_PIO_OFFSET);
    dipsw_pio     = (volatile uint32_t *)((uintptr_t)fpga_regs + DIPSW_PIO_OFFSET);
    irq_pio       = (volatile uint32_t *)((uintptr_t)fpga_regs + IRQ_PIO_OFFSET);
    
    /* ---- 4. Initial state ---- */
    *led_pio = LED_ALL_OFF;
    *pio_ctrl_reg = IRQ_MSG_IDLE;
    *pio_status_reg = IRQ_MSG_IDLE;
    
    /* Enable interrupts initially */
    enable_interrupts(irq_pio);
    
    printf("============================================================\n");
    printf("  HARDWARE HANDSHAKE INTERRUPT HANDLER\n");
    printf("============================================================\n");
    printf("  UIO Device: %s\n", UIO_DEVICE);
    printf("  LED PIO:    0x%08X\n", LED_PIO_BASE);
    printf("  CTRL Reg:   0x%08X\n", PIO_CTRL_REG_BASE);
    printf("  STATUS Reg: 0x%08X\n", PIO_STATUS_REG_BASE);
    printf("  IRQ PIO:    0x%08X\n", IRQ_PIO_BASE);
    printf("\n");
    printf("  Press a button to trigger an interrupt.\n");
    printf("  Each interrupt will be verified via hardware handshake.\n");
    printf("============================================================\n");
    printf("\n");
    
    /* ---- 5. Main ISR loop ---- */
    while (1) {
        /* Block until FPGA interrupt fires */
        if (read(fd, &irq_count, sizeof(irq_count)) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read failed");
            break;
        }
        
        total_interrupts++;
        
        /* ---- 6. Process interrupt with handshake ---- */
        int success = process_interrupt(irq_count,
                                        pio_ctrl_reg,
                                        pio_status_reg,
                                        led_pio,
                                        irq_pio,
                                        &led_pattern);
        
        if (!success) {
            handshake_failures++;
            printf("  ⚠️  Handshake FAILED for interrupt %u (Total failures: %u)\n", 
                   irq_count, handshake_failures);
        }
        
        /* ---- 7. Re-arm UIO interrupt ---- */
        if (write(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("write (re-arm) failed");
            break;
        }
    }
    
    /* ---- 8. Cleanup ---- */
    *led_pio = LED_ALL_OFF;
    *pio_ctrl_reg = IRQ_MSG_IDLE;
    *pio_status_reg = IRQ_MSG_IDLE;
    
    munmap((void *)fpga_regs, FPGA_REG_SIZE);
    close(fd);
    
    printf("\n============================================================\n");
    printf("  INTERRUPT HANDLER STOPPED\n");
    printf("  Total interrupts: %u\n", total_interrupts);
    printf("  Handshake failures: %u\n", handshake_failures);
    printf("============================================================\n");
    
    return 0;
}

//==========================================================================================================================================================================

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <errno.h>

#define UIO_DEVICE      "/dev/uio0"
#define FPGA_REG_SIZE   0x1000

/* Platform Designer Addresses (from your list) */
#define PIO_STATUS_REG_BASE     0x60    /* 0x00000060 - 0x0000006F */
#define PIO_CTRL_REG_BASE       0x40    /* 0x00000040 - 0x0000005F */
#define LED_PIO_BASE            0x10    /* 0x00000010 - 0x0000001F */
#define BUTTON_PIO_BASE         0x20    /* 0x00000020 - 0x0000002F */
#define DIPSW_PIO_BASE          0x30    /* 0x00000030 - 0x0000003F */
#define IRQ_PIO_BASE            0x40000 /* 0x00040000 - 0x0004000F */

/* Offsets relative to mmap base (which is 0xFF200000 for Lightweight H2F) */
#define LW_H2F_BASE             0xFF200000

/* Calculate absolute addresses for mmap region */
#define PIO_STATUS_REG_OFFSET   (PIO_STATUS_REG_BASE - LW_H2F_BASE)
#define PIO_CTRL_REG_OFFSET     (PIO_CTRL_REG_BASE - LW_H2F_BASE)
#define LED_PIO_OFFSET          (LED_PIO_BASE - LW_H2F_BASE)
#define BUTTON_PIO_OFFSET       (BUTTON_PIO_BASE - LW_H2F_BASE)
#define DIPSW_PIO_OFFSET        (DIPSW_PIO_BASE - LW_H2F_BASE)
#define IRQ_PIO_OFFSET          (IRQ_PIO_BASE - LW_H2F_BASE)

/* LED pattern defines (active-low: 0 = ON, 1 = OFF) */
#define LED_PATTERN_START       0xE     /* 1110 = LED0 ON, others OFF */
#define LED_ALL_OFF             0xF     /* 1111 = All OFF */
#define LED_ALL_ON              0x0     /* 0000 = All ON */

/* Interrupt message defines */
#define IRQ_MESSAGE_START       0xAA    /* Marker for interrupt start */
#define IRQ_MESSAGE_END         0x55    /* Marker for interrupt end */

/* Simple delay for SignalTap visibility (in microseconds) */
void micro_delay(useconds_t us) {
    usleep(us);
}

int main() {
    int fd;
    uint32_t irq_count = 0;
    volatile uint32_t *fpga_regs;
    volatile uint32_t *led_pio;
    volatile uint32_t *pio_ctrl_reg;
    volatile uint32_t *pio_status_reg;
    volatile uint32_t *button_pio;
    volatile uint32_t *dipsw_pio;
    volatile uint32_t *irq_pio;
    
    uint32_t led_pattern = LED_PATTERN_START;
    uint32_t interrupt_occurred = 0;
    
    /* ---- 1. Open UIO device ---- */
    fd = open(UIO_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open UIO device");
        return -1;
    }
    
    /* ---- 2. Memory-map FPGA register space ---- */
    fpga_regs = (volatile uint32_t *)mmap(NULL, FPGA_REG_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    
    if (fpga_regs == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;
    }
    
    /* ---- 3. Set up register pointers (relative to mmap base) ---- */
    led_pio       = (volatile uint32_t *)((uintptr_t)fpga_regs + LED_PIO_OFFSET);
    pio_ctrl_reg  = (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_CTRL_REG_OFFSET);
    pio_status_reg= (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_STATUS_REG_OFFSET);
    button_pio    = (volatile uint32_t *)((uintptr_t)fpga_regs + BUTTON_PIO_OFFSET);
    dipsw_pio     = (volatile uint32_t *)((uintptr_t)fpga_regs + DIPSW_PIO_OFFSET);
    irq_pio       = (volatile uint32_t *)((uintptr_t)fpga_regs + IRQ_PIO_OFFSET);
    
    /* ---- 4. Initial LED state ---- */
    *led_pio = LED_ALL_OFF;  // All LEDs OFF initially
    printf("Initial LEDs: All OFF\n");
    
    /* ---- 5. Clear control register ---- */
    *pio_ctrl_reg = 0x00;
    *pio_status_reg = 0x00;
    
    printf("========================================\n");
    printf("  Interrupt Handler Running...\n");
    printf("  UIO Device: %s\n", UIO_DEVICE);
    printf("  LED PIO Base: 0x%08X\n", LED_PIO_BASE);
    printf("  CTRL Reg Base: 0x%08X\n", PIO_CTRL_REG_BASE);
    printf("  STATUS Reg Base: 0x%08X\n", PIO_STATUS_REG_BASE);
    printf("  Press button to trigger interrupt!\n");
    printf("========================================\n");
    printf("\n");
    
    /* ---- 6. Main ISR loop ---- */
    while (1) {
        /* Block until FPGA interrupt fires */
        if (read(fd, &irq_count, sizeof(irq_count)) < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, retry
            }
            perror("read failed");
            break;
        }
        
        /* ================================================================
         * INTERRUPT RECEIVED! 
         * This is where the ISR (Interrupt Service Routine) executes
         * ================================================================ */
        
        interrupt_occurred = 1;
        
        /* ---- 7. Log interrupt ---- */
        printf("\n");
        printf("═══════════════════════════════════════════════════════════\n");
        printf("  🔔 INTERRUPT RECEIVED! Count: %u\n", irq_count);
        printf("═══════════════════════════════════════════════════════════\n");
        
        /* ---- 8. Read current button and DIP switch state ---- */
        uint32_t button_state = *button_pio;
        uint32_t dipsw_state = *dipsw_pio;
        printf("  Button State: 0x%02X  |  DIP Switch: 0x%02X\n", 
               button_state, dipsw_state);
        
        /* ---- 9. WRITE INTERRUPT MESSAGE TO CONTROL REGISTER ---- */
        /* Step 9a: Write start marker to control register (for SignalTap) */
        *pio_ctrl_reg = IRQ_MESSAGE_START;
        printf("  [WRITE] pio_ctrl_reg = 0x%02X (START marker)\n", IRQ_MESSAGE_START);
        
        /* Small delay for SignalTap to capture the write */
        micro_delay(1000);  // 1ms
        
        /* Step 9b: Write interrupt count to control register */
        *pio_ctrl_reg = irq_count & 0xFF;  // Lower 8 bits of count
        printf("  [WRITE] pio_ctrl_reg = 0x%02X (Count = %u)\n", 
               irq_count & 0xFF, irq_count);
        
        micro_delay(1000);
        
        /* Step 9c: Write end marker to control register */
        *pio_ctrl_reg = IRQ_MESSAGE_END;
        printf("  [WRITE] pio_ctrl_reg = 0x%02X (END marker)\n", IRQ_MESSAGE_END);
        
        micro_delay(1000);
        
        /* Step 9d: Write status register to confirm interrupt */
        *pio_status_reg = irq_count & 0xFF;
        printf("  [WRITE] pio_status_reg = 0x%02X\n", irq_count & 0xFF);
        
        /* ---- 10. TOGGLE LED PATTERN (Visual Confirmation) ---- */
        *led_pio = led_pattern;
        printf("  [WRITE] LED Pattern: 0x%02X (", led_pattern);
        
        /* Show which LEDs are ON */
        for (int i = 3; i >= 0; i--) {
            if ((led_pattern & (1 << i)) == 0) {
                printf("LED%d ON ", i);
            } else {
                printf("LED%d OFF ", i);
            }
        }
        printf(")\n");
        
        /* Rotate pattern for next interrupt (right-shift) */
        led_pattern = ((led_pattern << 1) & 0xF) | ((led_pattern >> 3) & 0x1);
        
        /* ---- 11. Write ACKNOWLEDGEMENT to UIO driver ---- */
        if (write(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("write (re-arm) failed");
            break;
        }
        printf("  [UIO] Interrupt re-armed (write count = %u)\n", irq_count);
        
        printf("═══════════════════════════════════════════════════════════\n");
        printf("\n");
        
        /* ---- 12. Wait for next interrupt ---- */
        /* Loop back to read() */
    }
    
    /* ---- 13. Cleanup ---- */
    *led_pio = LED_ALL_OFF;  // Turn all LEDs OFF
    *pio_ctrl_reg = 0x00;
    *pio_status_reg = 0x00;
    
    munmap((void *)fpga_regs, FPGA_REG_SIZE);
    close(fd);
    printf("\nInterrupt handler stopped.\n");
    
    return 0;
}















#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <errno.h>

#define UIO_DEVICE      "/dev/uio0"

/* ================================================================
 * Platform Designer Address Map — Arria 10 SoC
 * Offsets are directly from your Platform Designer address list.
 * The LW HPS-to-FPGA bridge base is 0xFF200000 on Arria 10.
 * mmap() maps from that base, so offsets below are used directly
 * as indices into the mapped region — NO subtraction needed.
 * ================================================================ */
#define LW_H2F_BASE             0xFF200000UL

#define LED_PIO_BASE            0x00000010  /* end: 0x0000001F */
#define BUTTON_PIO_BASE         0x00000020  /* end: 0x0000002F */
#define DIPSW_PIO_BASE          0x00000030  /* end: 0x0000003F */
#define PIO_CTRL_REG_BASE       0x00000040  /* end: 0x0000005F */
#define PIO_STATUS_REG_BASE     0x00000060  /* end: 0x0000006F */
#define IRQ_PIO_BASE            0x00040000  /* end: 0x0004000F */

/* PIO sub-register offsets (Intel/Altera standard for all PIO cores) */
#define PIO_DATA                0x00   /* read input / write output  */
#define PIO_DIRECTION           0x04   /* 1 = output, 0 = input      */
#define PIO_INTERRUPTMASK       0x08   /* 1 = IRQ enabled for that bit */
#define PIO_EDGECAPTURE         0x0C   /* write 1 to clear edge latch  */

/* mmap span must reach irq_pio end (0x4000F).
   Round up to next 4K page boundary = 0x41000.                     */
#define FPGA_REG_SIZE           0x41000

/* LED defines — active HIGH on Arria 10 DevKit (0xF = all ON) */
#define LED_ALL_OFF             0x0     /* 0000 = All OFF */
#define LED_ALL_ON              0xF     /* 1111 = All ON  */
#define LED_BLINK_MS            75      /* 75ms blink per interrupt   */

/* Interrupt message markers written to pio_ctrl_reg.
 * Signal Tap trigger: watch DATA reg go 0x00 -> 0xAA -> count -> 0x55 -> 0x00
 * That full sequence = one confirmed interrupt handled.             */
#define IRQ_MESSAGE_START       0xAA    /* interrupt occurred marker  */
#define IRQ_MESSAGE_END         0x55    /* interrupt handled marker   */

/* Helper: byte offset -> uint32_t array index */
#define REG(base_ptr, byte_offset) \
    (*((volatile uint32_t *)((uintptr_t)(base_ptr) + (byte_offset))))

/* Small delay for Signal Tap visibility */
void micro_delay(useconds_t us) {
    usleep(us);
}

int main() {
    int fd;
    uint32_t irq_count = 0;
    volatile uint32_t *fpga_regs;

    /* Individual PIO pointers — byte offsets from bridge base */
    volatile uint32_t *led_pio;
    volatile uint32_t *pio_ctrl_reg;
    volatile uint32_t *pio_status_reg;
    volatile uint32_t *button_pio;
    volatile uint32_t *dipsw_pio;
    volatile uint32_t *irq_pio;

    uint32_t led_pattern = 0xE;   /* rotate pattern: starts 1110 */

    /* ---- 1. Open UIO device ---- */
    fd = open(UIO_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Cannot open UIO device");
        return -1;
    }

    /* ---- 2. mmap the LW HPS-to-FPGA bridge ---- */
    fpga_regs = (volatile uint32_t *) mmap(NULL, FPGA_REG_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
    if (fpga_regs == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;
    }

    /* ---- 3. Set up register pointers using YOUR address offsets ----
     * Each offset is exactly as listed in your Platform Designer.
     * fpga_regs points to LW_H2F_BASE (0xFF200000).
     * Adding the offset in bytes gives the exact peripheral address. */
    led_pio        = (volatile uint32_t *)((uintptr_t)fpga_regs + LED_PIO_BASE);
    pio_ctrl_reg   = (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_CTRL_REG_BASE);
    pio_status_reg = (volatile uint32_t *)((uintptr_t)fpga_regs + PIO_STATUS_REG_BASE);
    button_pio     = (volatile uint32_t *)((uintptr_t)fpga_regs + BUTTON_PIO_BASE);
    dipsw_pio      = (volatile uint32_t *)((uintptr_t)fpga_regs + DIPSW_PIO_BASE);
    irq_pio        = (volatile uint32_t *)((uintptr_t)fpga_regs + IRQ_PIO_BASE);

    /* ---- 4. Initialise hardware to known state ---- */
    *led_pio       = LED_ALL_OFF;
    *pio_ctrl_reg  = 0x00;
    *pio_status_reg= 0x00;

    /* Enable interrupt on irq_pio bit 0, clear any stale edge */
    REG(irq_pio, PIO_INTERRUPTMASK) = 0x1;
    REG(irq_pio, PIO_EDGECAPTURE)   = 0x1;

    printf("========================================\n");
    printf("  Interrupt Handler Running — Arria 10 SoC\n");
    printf("  UIO Device      : %s\n", UIO_DEVICE);
    printf("  LED PIO         : 0x%08X\n", LED_PIO_BASE);
    printf("  CTRL Reg        : 0x%08X\n", PIO_CTRL_REG_BASE);
    printf("  STATUS Reg      : 0x%08X\n", PIO_STATUS_REG_BASE);
    printf("  IRQ PIO         : 0x%08X\n", IRQ_PIO_BASE);
    printf("  Signal Tap probe: pio_ctrl_reg @ 0x%08lX\n",
           LW_H2F_BASE + PIO_CTRL_REG_BASE);
    printf("  Waiting for interrupt...\n");
    printf("========================================\n\n");

    /* ---- 5. Main ISR loop ---- */
    while (1) {

        /* Blocks here until FPGA IRQ fires on irq_pio.
         * UIO kernel driver wakes this read() and returns
         * the cumulative interrupt count into irq_count.   */
        if (read(fd, &irq_count, sizeof(irq_count)) < 0) {
            if (errno == EINTR) continue;
            perror("read failed");
            break;
        }

        /* ============================================================
         * INTERRUPT RECEIVED
         * ============================================================ */

        printf("═══════════════════════════════════════════════════════════\n");
        printf("  INTERRUPT #%u RECEIVED\n", irq_count);
        printf("═══════════════════════════════════════════════════════════\n");

        /* ---- 6. Read button and DIP switch state ---- */
        uint32_t button_state = *button_pio;
        uint32_t dipsw_state  = *dipsw_pio;
        printf("  Button State : 0x%02X\n", button_state);
        printf("  DIP Switch   : 0x%02X\n", dipsw_state);

        /* ---- 7. Write message sequence to pio_ctrl_reg ----
         * Signal Tap: trigger on rising edge of DATA reg.
         * You will see this sequence per interrupt:
         *   0x00 -> 0xAA -> <count> -> 0x55 -> 0x00          */

        /* 7a: START marker — interrupt has occurred */
        *pio_ctrl_reg = IRQ_MESSAGE_START;
        printf("  [CTRL] pio_ctrl_reg <- 0x%02X  (START: interrupt occurred)\n",
               IRQ_MESSAGE_START);
        micro_delay(1000);  /* 1ms hold — visible in Signal Tap */

        /* 7b: Interrupt count — which interrupt number this is */
        *pio_ctrl_reg = irq_count & 0xFF;
        printf("  [CTRL] pio_ctrl_reg <- 0x%02X  (count = %u)\n",
               irq_count & 0xFF, irq_count);
        micro_delay(1000);

        /* 7c: END marker — ISR has handled the interrupt */
        *pio_ctrl_reg = IRQ_MESSAGE_END;
        printf("  [CTRL] pio_ctrl_reg <- 0x%02X  (END: interrupt handled)\n",
               IRQ_MESSAGE_END);
        micro_delay(1000);

        /* 7d: Write count to status reg — secondary confirmation */
        *pio_status_reg = irq_count & 0xFF;
        printf("  [STATUS] pio_status_reg <- 0x%02X\n", irq_count & 0xFF);

        /* ---- 8. Clear irq_pio EDGECAPTURE ----
         * MUST do this before re-arming UIO.
         * If not cleared, the PIO keeps asserting the IRQ line
         * and the next write(fd) fires immediately — you get
         * a flood of interrupts instead of one per event.      */
        uint32_t edge = REG(irq_pio, PIO_EDGECAPTURE);
        REG(irq_pio, PIO_EDGECAPTURE) = edge;
        printf("  [IRQ_PIO] EDGECAPTURE cleared (was 0x%X)\n", edge);

        /* ---- 9. Blink all 4 LEDs for 75ms ----
         * ON -> 75ms -> OFF
         * Signal Tap: watch LED_PIO DATA go 0x0 -> 0xF -> 0x0  */
        *led_pio = LED_ALL_ON;
        printf("  [LED] All 4 LEDs ON\n");
        micro_delay(LED_BLINK_MS * 1000);
        *led_pio = LED_ALL_OFF;
        printf("  [LED] All 4 LEDs OFF\n");

        /* ---- 10. Rotate LED pattern for next interrupt ---- */
        led_pattern = ((led_pattern << 1) & 0xF) | ((led_pattern >> 3) & 0x1);

        /* ---- 11. Clear ctrl_reg back to idle ---- */
        *pio_ctrl_reg  = 0x00;
        *pio_status_reg= 0x00;
        printf("  [CTRL] pio_ctrl_reg  <- 0x00  (idle)\n");
        printf("  [STATUS] pio_status_reg <- 0x00  (idle)\n");

        /* ---- 12. Re-arm UIO interrupt ----
         * Tells the UIO kernel driver to re-enable the IRQ at
         * the GIC so the next hardware event wakes read() again. */
        if (write(fd, &irq_count, sizeof(irq_count)) < 0) {
            perror("write (re-arm) failed");
            break;
        }
        printf("  [UIO] Interrupt re-armed. Waiting for next event...\n");
        printf("═══════════════════════════════════════════════════════════\n\n");
    }

    /* ---- 13. Cleanup ---- */
    *led_pio        = LED_ALL_OFF;
    *pio_ctrl_reg   = 0x00;
    *pio_status_reg = 0x00;
    munmap((void *)fpga_regs, FPGA_REG_SIZE);
    close(fd);
    printf("\nInterrupt handler stopped.\n");
    return 0;
}




























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
