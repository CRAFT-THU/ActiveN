// Simulation backdoor for the CIRCT-lowered Chisel ICache memories.
// Included by the simulator build, never by the synthesis RTL filelist.
module ICacheDpi;
  localparam int LINE_BYTES = `AN_ICACHE_BLOCK_SIZE;
  localparam int ASSOC = `AN_ICACHE_ASSOC;
  localparam int SETS = `AN_ICACHE_LINES / ASSOC;
  localparam int OFFSET_BITS = $clog2(LINE_BYTES);
  localparam int INDEX_BITS = $clog2(SETS);
  localparam int TAG_BITS = 32 - OFFSET_BITS - INDEX_BITS;

  export "DPI-C" function icacheLineBytes;
  function automatic int unsigned icacheLineBytes(); return LINE_BYTES; endfunction
  export "DPI-C" function icacheSets;
  function automatic int unsigned icacheSets(); return SETS; endfunction
  export "DPI-C" function icacheWays;
  function automatic int unsigned icacheWays(); return ASSOC; endfunction

  // Counting accepted requests (not presented valid cycles) also works under
  // backpressure and distinguishes memory beats from completed cache lines.
  longint unsigned refillRequests;
  longint unsigned globalRequests;
  longint unsigned lineFills;
  bit fetchStarted;
  always @(posedge ICache.clock) begin
    if (ICache.reset) begin
      refillRequests <= 0;
      globalRequests <= 0;
      lineFills <= 0;
      fetchStarted <= 0;
    end else begin
      if (ICache.input_ready && ICache.input_valid) fetchStarted <= 1;
      if (ICache.mem_req_valid && ICache.mem_req_ready)
        refillRequests <= refillRequests + 1;
      if (ICache.mem_req_valid && ICache.mem_req_ready && ICache.mem_req_bits_addr[31])
        globalRequests <= globalRequests + 1;
      if (ICache.s1refillComplete) lineFills <= lineFills + 1;
    end
  end

  export "DPI-C" function icacheRefillRequests;
  function automatic longint unsigned icacheRefillRequests();
    return refillRequests;
  endfunction

  export "DPI-C" function icacheGlobalRequests;
  function automatic longint unsigned icacheGlobalRequests();
    return globalRequests;
  endfunction

  export "DPI-C" function icacheLineFills;
  function automatic longint unsigned icacheLineFills();
    return lineFills;
  endfunction

  export "DPI-C" function icacheInitReady;
  function automatic bit icacheInitReady();
    return !ICache.reset && !ICache.s1reset && !ICache.s1valid && !fetchStarted;
  endfunction

  if (ASSOC > 1) begin : replacement
    task automatic touch(input int unsigned setIndex, input int unsigned way);
      bit [ASSOC-2:0] state;
      int node;
      bit direction;
      state = ICache.plruState_ext.Memory[setIndex];
      node = 0;
      for (int level = $clog2(ASSOC) - 1; level >= 0; --level) begin
        direction = 1'((way >> level) & 1);
        state[node] = !direction;
        node = 2 * node + 1 + int'(direction);
      end
      ICache.plruState_ext.Memory[setIndex] = state;
    endtask
  end else begin : replacement
    task automatic touch(input int unsigned setIndex, input int unsigned way);
    endtask
  end

  export "DPI-C" task insertLine;
  task automatic insertLine(input int unsigned address, input int unsigned way,
                            input bit [LINE_BYTES*8-1:0] lineData);
    int unsigned setIndex;
    bit [TAG_BITS-1:0] tagValue;
    if (!icacheInitReady())
      $fatal(1, "insertLine requires completed reset sweep and no captured fetch");
    if (way >= ASSOC || (address & (LINE_BYTES - 1)) != 0)
      $fatal(1, "insertLine requires an aligned address and an in-range way");
    setIndex = (address >> OFFSET_BITS) & (SETS - 1);
    tagValue = TAG_BITS'(address >> (OFFSET_BITS + INDEX_BITS));
    // A second matching way would make the cache's Mux1H ambiguous.
    for (int other = 0; other < ASSOC; ++other) begin
      if (other != way &&
          ICache.metadata_ext.Memory[setIndex][other*(TAG_BITS+1)+TAG_BITS] &&
          ICache.metadata_ext.Memory[setIndex][other*(TAG_BITS+1)+:TAG_BITS] == tagValue)
        $fatal(1, "insertLine would create duplicate matching tags");
    end
    for (int wordIndex = 0; wordIndex < LINE_BYTES / 4; ++wordIndex)
      ICache.data_ext.Memory[setIndex * (LINE_BYTES / 4) + wordIndex][way*32+:32]
        = lineData[wordIndex*32+:32];
    ICache.metadata_ext.Memory[setIndex][way*(TAG_BITS+1)+:(TAG_BITS+1)]
      = {1'b1, tagValue};
    replacement.touch(setIndex, way);
  endtask
endmodule

bind ICache ICacheDpi icache_dpi();
