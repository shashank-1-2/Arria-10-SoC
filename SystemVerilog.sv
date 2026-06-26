module interrupt_controller (
    input  logic        clk,          // System clock (from your design)
    input  logic        rst_n,        // Active-low reset
    // Inputs from your physical hardware (e.g., a button press)
    input  logic        hw_interrupt_src,
    // Avalon Memory-Mapped Interface (for HPS communication)
    input  logic        write_en,     // Write enable signal
    input  logic [31:0] write_data,   // Data to write (from HPS)
    input  logic        read_en,      // Read enable signal
    output logic [31:0] read_data,    // Data to read back (to HPS)
    // Interrupt output to HPS
    output logic        hps_irq       // The interrupt line for your HPS
);

    // Internal Registers
    logic [31:0] pio_ctrl_reg;        // This is your pio_ctrl_reg (0x40)
    logic [31:0] pio_status_reg;      // This is your pio_status_reg (0x60)
    logic [31:0] irq_count_reg;       // Internal interrupt counter
    logic        interrupt_pending;   // Internal flag for an unserviced interrupt
    logic        handshake_done;      // Set when software acknowledges
    logic [1:0]  handshake_state;     // State machine for handshake

    // -------------------------------------------------------------
    // Register Write Logic (from HPS)
    // -------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pio_ctrl_reg <= 32'h0;
            pio_status_reg <= 32'h0;
            handshake_state <= 2'b00;
            handshake_done <= 1'b0;
        end else if (write_en) begin
            // Write to the "Control Register" address (0x40)
            pio_ctrl_reg <= write_data;
            // The status register mirrors the control register for the handshake
            pio_status_reg <= write_data;

            // Simple handshake state machine for your C code to verify
            // and acknowledge the interrupt processing.
            if (write_data == 32'hAA) begin // START marker
                handshake_state <= 2'b01;
                handshake_done <= 1'b0;
            end else if (write_data == 32'h55) begin // END marker
                handshake_state <= 2'b10;
                handshake_done <= 1'b1; // Acknowledge that software has processed the IRQ
                // This signal 'handshake_done' can be used to clear the interrupt
            end
        end
    end

    // -------------------------------------------------------------
    // Interrupt Generation and Status Logic
    // -------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            irq_count_reg <= 32'h0;
            interrupt_pending <= 1'b0;
            hps_irq <= 1'b0;
        end else begin
            // 1. Detect the hardware interrupt source (e.g., rising edge of a button)
            if (hw_interrupt_src && !interrupt_pending) begin
                // Increment the interrupt counter
                irq_count_reg <= irq_count_reg + 1;
                // Set the interrupt pending flag
                interrupt_pending <= 1'b1;
                // Assert the interrupt line to the HPS
                hps_irq <= 1'b1;
            end

            // 2. Check if the HPS has finished processing (handshake_done from C code)
            if (handshake_done && interrupt_pending) begin
                // Clear the pending flag and de-assert the interrupt
                interrupt_pending <= 1'b0;
                hps_irq <= 1'b0;
            end

            // Optional: You can also use the handshake to store the count in a
            // specific register for the HPS to read, providing a mechanism to 
            // acknowledge that the IRQ was generated and the count was incremented.
        end
    end

    // -------------------------------------------------------------
    // Register Read Logic (to HPS)
    // -------------------------------------------------------------
    always_comb begin
        // Default read value
        read_data = 32'h0;
        if (read_en) begin
            // If the HPS reads from the status register address, it sees the count.
            // This mirrors the write handshake, allowing the HPS to verify.
            read_data = pio_status_reg;
        end
        // You can add logic here to return the interrupt count on a specific address.
    end

endmodule
