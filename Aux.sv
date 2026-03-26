module FPU (
  input logic clock,
  input logic reset,

  input logic [31:0] a,
  input logic [31:0] b,
  output logic [31:0] r,

  input logic [6:0] funct7,
  input logic [2:0] funct3,
  input logic rs2b0,

  input logic valid
);

  // Map RISC-V funct7/funct3 to fpnew operation, rounding mode, and op_mod
  logic [3:0] fp_op;
  logic [2:0] fp_rnd;
  logic       fp_op_mod;

  // Operands: fpnew expects 3 operands for FMA group.
  // For ADD: operands = {c (addend), b (multiplicand), a (multiplier)}
  //   actually for ADD op: result = operand[0]*operand[1] + operand[2], with op[0]=1.0
  //   so we set: operands[0] = 1.0 (handled internally), operands[1] = a, operands[2] = b
  //   Actually, fpnew_fma handles ADD specially: sets multiplicand to 1.0 internally
  //   For ADD: operands_i = {addend=b, operand_b=a, operand_a=a} - fpnew internally handles
  //   Looking at fpnew_fma.sv: for ADD, it sets operand_a = 1.0 internally
  //   So: operands[0] = don't care (replaced with 1.0), operands[1] = a, operands[2] = b
  // For MUL: operands[2] = +/-0 (handled internally)
  //   So: operands[0] = a, operands[1] = b, operands[2] = don't care

  logic [2:0][31:0] fp_operands;

  always_comb begin
    fp_op     = 4'd0; // FMADD
    fp_rnd    = 3'b000; // RNE
    fp_op_mod = 1'b0;
    fp_operands = {32'h0, b, a};

    // Map RISC-V rounding mode: 3'b111 (DYN) defaults to RNE (no FCSR)
    fp_rnd = (funct3 == 3'b111) ? 3'b000 : funct3;

    case (funct7)
      7'b0000000: begin // FADD.S: result = a + b
        fp_op = 4'd2; // ADD
        fp_operands[0] = a;
        fp_operands[1] = a;   // operand_b = a (fpnew replaces op_a with 1.0 for ADD)
        fp_operands[2] = b;   // operand_c = b (addend)
      end
      7'b0000100: begin // FSUB.S: result = a - b
        fp_op = 4'd2; // ADD
        fp_op_mod = 1'b1; // subtraction
        fp_operands[0] = a;
        fp_operands[1] = a;
        fp_operands[2] = b;
      end
      7'b0001000: begin // FMUL.S: result = a * b
        fp_op = 4'd3; // MUL
        fp_operands[0] = a;
        fp_operands[1] = b;
        fp_operands[2] = 32'h0;
      end
      7'b0010000: begin // FSGNJ/FSGNJN/FSGNJX
        fp_op = 4'd6; // SGNJ
        fp_operands[0] = a;
        fp_operands[1] = b;
        fp_operands[2] = 32'h0;
      end
      7'b0010100: begin // FMIN/FMAX
        fp_op = 4'd7; // MINMAX
        fp_operands[0] = a;
        fp_operands[1] = b;
        fp_operands[2] = 32'h0;
      end
      7'b1100000: begin // FCVT.W[U].S (FP to Int)
        fp_op = 4'd11; // F2I
        fp_op_mod = rs2b0;
        fp_operands[0] = a;
        fp_operands[1] = 32'h0;
        fp_operands[2] = 32'h0;
      end
      7'b1010000: begin // FEQ/FLT/FLE
        fp_op = 4'd8; // CMP
        fp_operands[0] = a;
        fp_operands[1] = b;
        fp_operands[2] = 32'h0;
      end
      7'b1101000: begin // FCVT.S.W[U] (Int to FP)
        fp_op = 4'd12; // I2F
        fp_op_mod = rs2b0;
        fp_operands[0] = a;
        fp_operands[1] = 32'h0;
        fp_operands[2] = 32'h0;
      end
      default: begin
        fp_op = 4'd2;
        fp_operands = {32'h0, 32'h0, 32'h0};
      end
    endcase
  end

  logic fpnew_out_valid;
  logic fpnew_in_ready;
  logic [31:0] fpnew_result;

  fpnew_top #(
    .Features       ( fpnew_pkg::RV32F ),
    .Implementation ( '{
      PipeRegs:   '{default: 0},  // Fully combinational
      UnitTypes:  '{'{default: fpnew_pkg::PARALLEL},  // ADDMUL
                    '{default: fpnew_pkg::DISABLED},   // DIVSQRT (not needed)
                    '{default: fpnew_pkg::PARALLEL},   // NONCOMP
                    '{default: fpnew_pkg::MERGED}},    // CONV
      PipeConfig: fpnew_pkg::AFTER
    }),
    .TagType        ( logic ),
    .TrueSIMDClass  ( 0 ),
    .EnableSIMDMask ( 0 )
  ) i_fpnew (
    .clk_i          ( clock ),
    .rst_ni         ( ~reset ),
    .operands_i     ( fp_operands ),
    .rnd_mode_i     ( fpnew_pkg::roundmode_e'(fp_rnd) ),
    .op_i           ( fpnew_pkg::operation_e'(fp_op) ),
    .op_mod_i       ( fp_op_mod ),
    .src_fmt_i      ( fpnew_pkg::FP32 ),
    .dst_fmt_i      ( fpnew_pkg::FP32 ),
    .int_fmt_i      ( fpnew_pkg::INT32 ),
    .vectorial_op_i ( 1'b0 ),
    .tag_i          ( 1'b0 ),
    .simd_mask_i    ( 1'b1 ),
    .in_valid_i     ( valid ),
    .in_ready_o     ( fpnew_in_ready ),
    .flush_i        ( 1'b0 ),
    .result_o       ( fpnew_result ),
    .status_o       ( ),
    .tag_o          ( ),
    .out_valid_o    ( fpnew_out_valid ),
    .out_ready_i    ( 1'b1 ),
    .busy_o         ( ),
    .early_valid_o  ( )
  );

  // Output holding register: captures fpnew result when valid, holds it otherwise.
  // This provides the 1-cycle latency expected by the exec pipeline.
  reg [31:0] r_held;
  always_ff @(posedge clock) begin
    if (reset)
      r_held <= 32'h0;
    else if (valid)
      r_held <= fpnew_result;
  end
  assign r = r_held;

endmodule
