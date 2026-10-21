// Read-only simulation access to the CIRCT-lowered SPM array.
// Compiled by the simulator, outside the synthesis RTL filelist.
module SpmDpi;
  localparam int LINE_BYTES = `AN_SPM_LINE_BYTES;

  // Report the actual lowered geometry so the host can check its configuration.
  export "DPI-C" function spmSizeBytes;
  function automatic int unsigned spmSizeBytes();
    return $size(SPM.scratchpad_ext.Memory) * $bits(SPM.scratchpad_ext.Memory[0]) / 8;
  endfunction

  export "DPI-C" function spmLineBytes;
  function automatic int unsigned spmLineBytes();
    return $bits(SPM.scratchpad_ext.Memory[0]) / 8;
  endfunction

  export "DPI-C" function spmReadWord;
  function automatic int unsigned spmReadWord(input int unsigned byteOffset);
    return SPM.scratchpad_ext.Memory[byteOffset / LINE_BYTES]
                                  [(byteOffset % LINE_BYTES)*8 +: 32];
  endfunction
endmodule

bind SPM SpmDpi spm_dpi();
