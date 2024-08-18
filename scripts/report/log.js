#!/usr/bin/env node

const fs = require('fs');
const path = require('path');
const content = fs.readFileSync(path.resolve(process.argv[2], './log.txt')).toString('utf-8').split('\n').map(e => e.trim()).filter(e => !!e);

const divisor = parseFloat(process.argv[3]);

// Test environment have an extra 175 cycle for setup (before init in assembly). This procedure only executes on first startup, not subsequent rounds.
// Instead, this is replaced with a startup message which takes 32 cycles in runtime.
const TEST_OVERHEAD_CYCLES = 175 - 32;

let totCyles = NaN;

for(const row of content) {
  const match = row.match(/Core Cycles: (\d+)$/);
  if(!match) continue;
  const cycles = parseInt(match[1]) - TEST_OVERHEAD_CYCLES;
  totCycles = cycles;
}

const idleRatios = content.map(e => e.match(/Avg idle cycle:.*= ([0-9.]+)$/)).filter(e => !!e);
const workingRatios = content.map(e => e.match(/Avg working ratio:.*= ([0-9.]+)$/)).filter(e => !!e);

if(process.argv[4] !== 'simple') {
  console.log('Cores:');
  console.log(`  Total time (ms): ${(totCycles / divisor).toFixed(2)}`);
  console.log(`  Occupancy: ${(100 - parseFloat(idleRatios[idleRatios.length - 1][1]) * 100).toFixed(5)}%`)
  console.log(`  FU Util: ${(parseFloat(workingRatios[idleRatios.length - 1][1]) * 100).toFixed(5)}%`)
} else {
  console.log(`${(totCycles / divisor).toFixed(3)}\t`
    + `${(100 - parseFloat(idleRatios[idleRatios.length - 1][1]) * 100).toFixed(3)}%\t`
    + `${(parseFloat(workingRatios[idleRatios.length - 1][1]) * 100).toFixed(3)}%\t`);
}
