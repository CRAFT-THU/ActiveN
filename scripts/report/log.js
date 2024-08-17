#!/usr/bin/env node

console.log('Cores:');

const fs = require('fs');
const path = require('path');
const content = fs.readFileSync(path.resolve(process.argv[2], './log.txt')).toString('utf-8').split('\n').map(e => e.trim()).filter(e => !!e);

const divisor = parseFloat(process.argv[3]);

// Test environment have an extra 175 cycle for setup (before init in assembly). This procedure only executes on first startup, not subsequent rounds.
// Instead, this is replaced with a startup message which takes 32 cycles in runtime.
const TEST_OVERHEAD_CYCLES = 175 - 32;

for(const row of content) {
  const match = row.match(/Core Cycles: (\d+)$/);
  if(!match) continue;
  const cycles = parseInt(match[1]) - TEST_OVERHEAD_CYCLES;
  console.log(`  Total time (ms): ${cycles / divisor}`);
}

const idleRatios = content.map(e => e.match(/Avg idle cycle:.*= ([0-9.]+)$/)).filter(e => !!e);
const workingRatios = content.map(e => e.match(/Avg working ratio:.*= ([0-9.]+)$/)).filter(e => !!e);

console.log(`  Idle thread: ${parseFloat(idleRatios[idleRatios.length - 1][1]) * 100}%`)
console.log(`  Idle core: ${100 - parseFloat(workingRatios[idleRatios.length - 1][1]) * 100}%`)
