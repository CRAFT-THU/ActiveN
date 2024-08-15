#!/usr/bin/env node

const fs = require('fs');
const path = require('path');
const content = fs.readFileSync(path.resolve(process.argv[2], './log.txt')).toString('utf-8').split('\n').map(e => e.trim()).filter(e => !!e);

const divisor = parseFloat(process.argv[3]);

for(const row of content) {
  const match = row.match(/Core Cycles: (\d+)$/);
  if(!match) continue;
  const cycles = parseInt(match[1])
  console.log(`Total time (ms): ${cycles / divisor}`);
}

const idleRatios = content.map(e => e.match(/Avg idle cycle:.*= ([0-9.]+)$/)).filter(e => !!e);
const workingRatios = content.map(e => e.match(/Avg working ratio:.*= ([0-9.]+)$/)).filter(e => !!e);

console.log(`Idle thread: ${parseFloat(idleRatios[idleRatios.length - 1][1]) * 100}%`)
console.log(`Idle core: ${100 - parseFloat(workingRatios[idleRatios.length - 1][1]) * 100}%`)
