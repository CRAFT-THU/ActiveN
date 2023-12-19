import "DPI-C" function longint softfpu_compute(input logic [31:0] a, input logic [31:0] b, input logic [6:0] funct7, input logic [2:0] funct3, input logic rs2b0);
import "DPI-C" function int softfpu_delay(input logic [6:0] funct7, input logic [2:0] funct3);

import "DPI-C" function void softmem_write(input logic [31:0] hartid, input logic [31:0] addr, input logic [31:0] wdata, input [3:0] we);
import "DPI-C" function longint softmem_read(input logic [31:0] hartid, input logic [31:0] addr);

module FPU (
  input logic clock,
  input logic reset,

  input logic [31:0] a,
  input logic [31:0] b,
  output logic [31:0] r,

  input logic [6:0] funct7,
  input logic [2:0] funct3,
  input logic rs2b0,

  input logic valid,
  output logic ready
);
  int cnt;
  reg is_ready;
  reg [31:0] result;

  assign ready = is_ready;
  assign r = result;

  always_ff @(posedge clock or posedge reset) begin // Update data on negedge
    if(cnt == 0 && valid && !reset) begin
      result <= softfpu_compute(a, b, funct7, funct3, rs2b0);
    end

    if(!reset && valid && !is_ready) begin
      is_ready <= (cnt + 1) == softfpu_delay(funct7, funct3);
      cnt <= cnt + 1;
    end else begin
      is_ready <= '0;
      cnt <= 0;
    end
  end
endmodule

module SPM (
  input logic clock,
  input logic [31:0] addr,
  input logic [3:0] we,
  input logic [31:0] wdata,
  
  input logic [31:0] hartid,
  output logic [31:0] data
);
  reg [31:0] rdata;
  always @(posedge clock or negedge clock or addr) begin
    rdata <= softmem_read(hartid, addr);
  end

  always_ff @(posedge clock) begin
    softmem_write(hartid, addr, wdata, we);
  end
  assign data = rdata;
endmodule
