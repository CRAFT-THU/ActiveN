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
);
  reg [31:0] result;
  reg [31:0] holding;

  assign r = result;

  always_ff @(posedge clock or posedge reset) begin // Update data on negedge
    if(!reset) begin
      holding <= valid ? 0'hdeadbeef : softfpu_compute(a, b, funct7, funct3, rs2b0);
      result <= holding;
    end else begin
      holding <= 0'hdeadbeef;
      result <= 0'hdeadbeef;
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
