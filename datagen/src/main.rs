use std::collections::HashSet;
use std::{collections::HashMap, fs::File, path::PathBuf};

use byteorder::LittleEndian;
use byteorder::WriteBytesExt;
use clap::Parser;
use rand::seq::SliceRandom;
use rand::{Rng, SeedableRng};
use rand_distr::{Distribution, Geometric, Uniform};
use rand_xoshiro::Xoshiro256PlusPlus;
use serde::Deserialize;
use std::io::BufReader;
use std::io::BufWriter;
use std::io::Write;

const XOR_SCALE: f32 = 10.0;
const CSR_ROW_ALIGN_WORDS: usize = 8;

fn round_ties_even_i64(value: f64) -> i64 {
    let lower = value.floor();
    let frac = value - lower;
    let rounded = if frac < 0.5 {
        lower
    } else if frac > 0.5 {
        lower + 1.0
    } else if (lower as i64) & 1 == 0 {
        lower
    } else {
        lower + 1.0
    };
    rounded as i64
}

fn quantize_xor_word(value: f32) -> u32 {
    if !value.is_finite() {
        return value.to_bits();
    }
    let scaled = value * XOR_SCALE;
    round_ties_even_i64(scaled as f64) as u32
}

fn pad_csr_row(words: &mut Vec<u32>) {
    while words.len() % CSR_ROW_ALIGN_WORDS != 0 {
        words.push(0);
        words.push(0);
    }
}

#[derive(Parser)]
struct Args {
    #[clap(short, long)]
    core_cnt: usize,

    #[clap(long, default_value = "1")]
    num_mc: usize,

    #[clap(long)]
    load_nest_nodes: Option<PathBuf>,

    #[clap(long)]
    load_nest_conns: Option<PathBuf>,

    #[clap(long)]
    nest_randomize: bool,

    // #[clap(short, long, default_value="1000")]
    // neuron_per_core: usize,
    #[clap(short, long, default_value = "82601")]
    tot_neuron: usize,

    #[clap(long, default_value = "0.081167")]
    connectivity: f64,

    #[clap(short, long, default_value = "100")]
    pre_simulate: usize,

    #[clap(long, default_value = "0.1")]
    tau: f32,

    #[clap(long, default_value = "15")]
    threshold: f32,

    #[clap(long, default_value = "19260817")]
    seed: u64,

    #[clap(long, default_value = "0.273786")]
    inh_ratio: f64,

    #[clap(short, long)]
    dump: Option<PathBuf>,

    #[clap(long, default_value = "0x80100000")]
    dram_base: String,

    #[clap(long)]
    dump_genn: Option<PathBuf>,

    #[clap(long)]
    sudoku: bool,

    #[clap(long)]
    mnist: Option<f64>,
}

#[derive(Clone)]
struct Neigh {
    core: u16,
    neuron: u16,
    weight: f32,
}

#[derive(Clone)]
struct Neuron {
    state: f32,
    input: f32,
    inj: f32,
    neigh: Vec<Neigh>,
}

#[derive(Default, Clone)]
struct Core {
    neurons: Vec<Neuron>,
}

fn dump(base: &PathBuf, cores: &Vec<Core>, dram_base: u32, num_mc: usize) -> anyhow::Result<()> {
    println!("Dumping to {}", base.display());

    let core_cnt = cores.len();
    let spm_size: u32 = 16384;
    let pus_per_mc = core_cnt / num_mc;

    // New SPM format: per neuron = state(f32), input(f32), csr_start_mc[0..num_mc-1]
    // Plus 1 sentinel neuron at the end with past-the-end CSR addresses
    let words_per_neuron = 2 + num_mc; // state, input, N x csr_start

    // Layout in the combined dram.0 file:
    //   [0, desc_end):        Descriptor table (core_cnt * 8 bytes)
    //   [desc_end, spm_end):  SPM initializer blocks (core_cnt * spm_size)
    //   [spm_end, ...):       CSR data for each MC (concatenated, each MC's block is separate)
    let desc_size = (core_cnt as u32) * 8;
    let spm_total = (core_cnt as u32) * spm_size;
    let csr_file_offset = desc_size + spm_total;

    // Step 1: Partition CSR entries by destination MC
    // csr_per_mc[mc] = Vec<u32> of CSR entries (col, weight pairs) for that MC
    let mut csr_per_mc: Vec<Vec<u32>> = vec![Vec::new(); num_mc];
    // per_neuron_csr_starts[ci][ni][mc] = byte offset within csr_per_mc[mc] where this neuron's row starts
    let mut per_neuron_csr_starts: Vec<Vec<Vec<u32>>> = Vec::new();

    for ci in 0..core_cnt {
        let c = &cores[ci];
        let mut neuron_starts: Vec<Vec<u32>> = Vec::new();
        for n in c.neurons.iter() {
            // Record start offsets for each MC
            let starts: Vec<u32> = (0..num_mc).map(|mc| (csr_per_mc[mc].len() * 4) as u32).collect();
            neuron_starts.push(starts);

            // Partition neighbors by target MC
            for neigh in n.neigh.iter() {
                let target_pu = neigh.core as usize; // 0-based core index
                let target_mc = target_pu / pus_per_mc;
                debug_assert!(target_mc < num_mc, "target_mc={} >= num_mc={}", target_mc, num_mc);
                // Core IDs are 1-based in the new system (PU IDs 1..core_cnt)
                let col = ((neigh.core as u32 + 1) << 16) | (neigh.neuron as u32);
                csr_per_mc[target_mc].push(col);
                csr_per_mc[target_mc].push(neigh.weight.to_bits());
            }

            // Scatter reads whole 256-bit beats without a per-entry valid mask,
            // so each per-neuron row must end on a beat boundary.
            for mc in 0..num_mc {
                pad_csr_row(&mut csr_per_mc[mc]);
            }
        }
        // Sentinel: past-the-end for last neuron
        let sentinel: Vec<u32> = (0..num_mc).map(|mc| (csr_per_mc[mc].len() * 4) as u32).collect();
        neuron_starts.push(sentinel);
        per_neuron_csr_starts.push(neuron_starts);
    }

    // Step 2: Compute CSR base addresses per MC
    // All CSR data goes into the dram.0 file sequentially after SPM data.
    // Each MC's CSR block is at a known file offset.
    // The MC-local address = (file_offset_of_mc_csr_block + entry_offset_within_block)
    //   because the Encoder converts global addresses to MC-local by subtracting MC's base.
    //   For MC0: global = dram_base + file_offset, local = global - 0x80000000
    //   For MC1: global = dram_base + file_offset, but MC1's DRAM starts at 0x80000000 + mc_size
    //   So local = global - (0x80000000 + mc_size) = dram_base - 0x80000000 + file_offset - mc_size
    //
    // Since all data is in a flat file loaded at dram_base (0x80100000),
    // and the simulator adjusts per-MC addressing:
    //   MC0 local_addr → global = local_addr + 0x80000000
    //   MC1 local_addr → global = local_addr + 0x80000000 + mc_size
    //
    // So for MC0: CSR MC-local addr = (dram_base - 0x80000000) + file_offset
    // For MC1: CSR MC-local addr = (dram_base - 0x80000000) + file_offset - mc_size
    // Generalization: MC-local addr = (dram_base - 0x80000000 - mc_cumul_base) + file_offset

    let mc_size: u64 = 0x100000000; // 4 GiB per MC (matching memCtrlSizes)
    let mut csr_mc_file_offsets: Vec<u32> = Vec::new();
    let mut cur_offset = csr_file_offset;
    for mc in 0..num_mc {
        csr_mc_file_offsets.push(cur_offset);
        cur_offset += (csr_per_mc[mc].len() * 4) as u32;
    }

    // MC-local base for each MC's CSR block
    let dram_offset_from_text = dram_base - 0x80000000u32;
    let csr_mc_local_bases: Vec<u32> = (0..num_mc).map(|mc| {
        let mc_cumul_base = (mc as u64) * mc_size;
        // MC-local address of the start of this MC's CSR block
        ((dram_offset_from_text as u64 + csr_mc_file_offsets[mc] as u64) - mc_cumul_base) as u32
    }).collect();

    // Step 3: Build SPM data
    let mut all_spm: Vec<Vec<u32>> = Vec::new();
    for ci in 0..core_cnt {
        let c = &cores[ci];
        let nn_count = c.neurons.len();
        let mut spm: Vec<u32> = Vec::new();

        // Regular neurons
        for ni in 0..nn_count {
            let n = &c.neurons[ni];
            spm.push(n.state.to_bits());
            spm.push(n.input.to_bits());
            for mc in 0..num_mc {
                // MC-local start address for this neuron's CSR row in this MC
                spm.push(csr_mc_local_bases[mc] + per_neuron_csr_starts[ci][ni][mc]);
            }
        }
        // Sentinel neuron (past-the-end CSR addresses)
        spm.push(0); // state placeholder
        spm.push(0); // input placeholder
        for mc in 0..num_mc {
            spm.push(csr_mc_local_bases[mc] + per_neuron_csr_starts[ci][nn_count][mc]);
        }

        spm.resize(spm_size as usize / 4, 0);
        // nn_count at word 4095
        spm[spm_size as usize / 4 - 1] = nn_count as u32;      // +0x3FFC: nn_count
        // +0x3FF8: init state (written by system.cpp preloader, not datagen)
        // +0x3FF4: sync counter (used at runtime)
        spm[spm_size as usize / 4 - 4] = num_mc as u32;       // +0x3FF0: num_mc
        spm[spm_size as usize / 4 - 5] = (words_per_neuron * 4) as u32; // +0x3FEC: neuron stride
        spm[spm_size as usize / 4 - 6] = core_cnt as u32;     // +0x3FE8: num_pu
        all_spm.push(spm);
    }

    // Step 4: Write combined dram.0 file
    let mut out_file = base.clone();
    out_file.push("dram.0");
    let mut writer = BufWriter::new(File::create(&out_file)?);

    // 1. Descriptor table
    // Each entry: (spm_src: u32, neuron_data_bytes: u32)
    // neuron_data_bytes = (nn_count + 1) * words_per_neuron * 4 (includes sentinel)
    for ci in 0..core_cnt {
        let spm_src = dram_base + desc_size + (ci as u32) * spm_size;
        let nn_data_bytes = ((cores[ci].neurons.len() + 1) as u32) * (words_per_neuron as u32) * 4;
        writer.write_u32::<LittleEndian>(spm_src)?;
        writer.write_u32::<LittleEndian>(nn_data_bytes)?;
    }

    // 2. SPM initializer blocks
    for spm in &all_spm {
        for word in spm {
            writer.write_u32::<LittleEndian>(*word)?;
        }
    }

    // 3. CSR data for each MC
    for mc in 0..num_mc {
        for word in &csr_per_mc[mc] {
            writer.write_u32::<LittleEndian>(*word)?;
        }
    }

    writer.flush()?;
    let total_csr: usize = csr_per_mc.iter().map(|v| v.len() * 4).sum();
    let total_size = desc_size as usize + spm_total as usize + total_csr;
    println!("  dram.0: {} bytes (desc={}, spm={}, csr={})",
        total_size, desc_size, spm_total, total_csr);
    for mc in 0..num_mc {
        println!("    MC{}: csr={} bytes, local_base=0x{:08x}",
            mc, csr_per_mc[mc].len() * 4, csr_mc_local_bases[mc]);
    }

    Ok(())
}

fn dump_genn(base: &PathBuf, cores: &Vec<Core>) -> anyhow::Result<()> {
    println!("Dumping(GeNN) to {}", base.display());

    let mut output = BufWriter::new(File::create(base)?);

    let mut prefix_sum = Vec::with_capacity(cores.len());
    let mut prefix_sum_cur = 0;
    for c in cores.iter() {
        prefix_sum.push(prefix_sum_cur);
        prefix_sum_cur += c.neurons.len();
    }

    output.write_u32::<LittleEndian>(prefix_sum_cur as u32)?;
    for c in cores.iter() {
        for n in c.neurons.iter() {
            output.write_f32::<LittleEndian>(n.state)?;
        }
    }
    for c in cores.iter() {
        for n in c.neurons.iter() {
            output.write_f32::<LittleEndian>(n.input)?;
        }
    }

    // Actually CSR
    let mut max_syn = 0;
    for c in cores.iter() {
        for n in c.neurons.iter() {
            output.write_u32::<LittleEndian>(n.neigh.len() as u32)?;
            max_syn = max_syn.max(n.neigh.len());
        }
    }
    println!("Max syn cnt: {}", max_syn);

    for c in cores.iter() {
        for n in c.neurons.iter() {
            let mut cnt = 0;
            for neigh in n.neigh.iter() {
                output.write_u32::<LittleEndian>(
                    prefix_sum[neigh.core as usize] as u32 + neigh.neuron as u32,
                )?;
                cnt += 1;
            }

            for _ in cnt..max_syn {
                output.write_u32::<LittleEndian>(0)?;
            }
        }
    }

    for c in cores.iter() {
        for n in c.neurons.iter() {
            let mut cnt = 0;
            for neigh in n.neigh.iter() {
                output.write_f32::<LittleEndian>(neigh.weight)?;
                cnt += 1;
            }

            for _ in cnt..max_syn {
                output.write_u32::<LittleEndian>(0)?;
            }
        }
    }

    Ok(())
}

#[derive(Deserialize)]
struct NestNode {
    s: f32,
    t: f32,
    r: f32,
    id: usize,
}

#[derive(Deserialize)]
struct NestConn {
    s: usize,
    t: usize,
    w: f32,
}

struct SudokuIterator {
    x: usize,
    y: usize,
    digit: usize,
    sub: usize,
    pop: usize,
}

impl SudokuIterator {
    fn new(pop: usize) -> Self {
        Self {
            x: 0,
            y: 0,
            digit: 0,
            sub: 0,
            pop,
        }
    }
}

impl Iterator for SudokuIterator {
    type Item = (usize, usize, usize, usize);

    fn next(&mut self) -> Option<Self::Item> {
        if self.x >= 9 {
            return None;
        }

        let ret = (self.x, self.y, self.digit, self.sub);
        if self.sub < self.pop - 1 {
            self.sub += 1;
            return Some(ret);
        }
        self.sub = 0;

        if self.digit < 8 {
            self.digit += 1;
            return Some(ret);
        }
        self.digit = 0;

        if self.y < 8 {
            self.y += 1;
            return Some(ret);
        }
        self.y = 0;

        self.x += 1;
        return Some(ret);
    }
}

fn main() -> anyhow::Result<()> {
    let mut args = Args::parse();

    if args.sudoku {
        assert_eq!(args.tot_neuron % (9 * 9 * 9), 0);
    }

    if args.mnist.is_some() {
        assert_eq!(args.tot_neuron, 28 * 28 + 10);
        assert_eq!(args.pre_simulate, 1);
    }

    let mut rng = Xoshiro256PlusPlus::seed_from_u64(args.seed);

    let e_neg_tau = std::f32::consts::E.powf(-args.tau);

    let neigh_dist = Geometric::new(args.connectivity)?;
    let ext_weight_dist = Uniform::new(0.85, 0.92);
    let inh_weight_dist = Uniform::new(-2.5f32, -2.44);
    let inj_weight_dist = Uniform::new(0.3, 0.8);
    let init_state_dist = Uniform::new(0f32, args.threshold);

    let mut nest_nodes: Option<Vec<NestNode>> = args
        .load_nest_nodes
        .as_ref()
        .map(|p| serde_json::from_reader(BufReader::new(std::fs::File::open(p).unwrap())).unwrap());
    let nest_conns: Option<Vec<NestConn>> = args
        .load_nest_conns
        .as_ref()
        .map(|p| serde_json::from_reader(BufReader::new(std::fs::File::open(p).unwrap())).unwrap());

    if let Some(ref mut nodes) = nest_nodes {
        args.tot_neuron = nodes.len();
        if args.nest_randomize {
            nodes.as_mut_slice().shuffle(&mut rng);
        }
    }

    let mut core_nn_cnt = vec![args.tot_neuron / args.core_cnt; args.core_cnt];
    for i in 0..(args.tot_neuron % args.core_cnt) {
        core_nn_cnt[i] += 1;
    }

    // Generate
    let mut cores: Vec<Core> = vec![Default::default(); args.core_cnt];
    let mut max_syn_per_neuron = 0;

    let mut nest_iter = 0;
    let mut nest_rev_map: HashMap<usize, (usize, usize)> = HashMap::new();

    let mut sudoku_rev_map: HashMap<(usize, usize, usize, usize), (usize, usize)> = HashMap::new();

    for core in 0..args.core_cnt {
        println!("Gen: core {}", core);
        cores[core].neurons.reserve(core_nn_cnt[core]);
        if let Some(ref nodes) = nest_nodes {
            for nid in 0..core_nn_cnt[core] {
                assert!(nest_iter < nodes.len());
                let node = &nodes[nest_iter];

                nest_rev_map.insert(node.id, (core, nid));
                nest_iter += 1;

                let state_ratio = (node.s - node.r) / (node.t - node.r);
                let state = state_ratio * args.threshold;

                let init_firing_rate = 0.1;
                let fired = rng.gen_bool(init_firing_rate);

                cores[core].neurons.push(Neuron {
                    state,
                    input: if fired { 1000000000f32 } else { 0f32 },
                    inj: 0f32,
                    neigh: Vec::new(),
                })
            }
        } else if !args.sudoku && args.mnist.is_none() {
            for _ in 0..core_nn_cnt[core] {
                let is_inh = rng.gen_bool(args.inh_ratio);

                let mut neigh = Vec::new();
                let mut cur = 0u64;
                let mut cur_core = 0usize;

                loop {
                    cur += neigh_dist.sample(&mut rng);

                    while cur_core < args.core_cnt && cur as usize >= core_nn_cnt[cur_core] {
                        cur -= core_nn_cnt[cur_core] as u64;
                        cur_core += 1;
                    }
                    if cur_core >= args.core_cnt {
                        break;
                    }

                    let w = if is_inh {
                        inh_weight_dist
                    } else {
                        ext_weight_dist
                    }
                    .sample(&mut rng);
                    neigh.push(Neigh {
                        core: cur_core as u16,
                        neuron: cur as u16,
                        weight: w as f32,
                    });
                    cur += 1;
                }

                max_syn_per_neuron = max_syn_per_neuron.max(neigh.len());
                let inj = inj_weight_dist.sample(&mut rng);

                cores[core].neurons.push(Neuron {
                    state: init_state_dist.sample(&mut rng),
                    input: inj,
                    inj,
                    neigh,
                })
            }
        }
    }

    if args.sudoku {
        let pop = args.tot_neuron / (9 * 9 * 9);
        let mut it: SudokuIterator = SudokuIterator::new(pop);
        for c in 0..args.core_cnt {
            for _ in 0..core_nn_cnt[c] {
                let ident = it.next().unwrap();

                sudoku_rev_map.insert(ident, (c, cores[c].neurons.len()));

                cores[c].neurons.push(Neuron {
                    state: init_state_dist.sample(&mut rng) * 2.0,
                    input: 0f32,
                    inj: 0f32,
                    neigh: Vec::new(),
                });
            }
        }

        assert_eq!(it.next(), None);
        let mut coll = Vec::new();

        for (ident, at) in sudoku_rev_map.iter() {
            coll.clear();

            // Ext
            for sub in 0..pop {
                let sub_at = sudoku_rev_map
                    .get(&(ident.0, ident.1, ident.2, sub))
                    .unwrap();
                coll.push((sub_at.0, sub_at.1, true));
            }

            // Inh
            for digit in 0..9 {
                if digit != ident.2 {
                    for sub in 0..pop {
                        let sub_at = sudoku_rev_map.get(&(ident.0, ident.1, digit, sub)).unwrap();
                        coll.push((sub_at.0, sub_at.1, false));
                    }
                }
            }
            for x in 0..9 {
                if x != ident.0 {
                    for sub in 0..pop {
                        let sub_at = sudoku_rev_map.get(&(x, ident.1, ident.2, sub)).unwrap();
                        coll.push((sub_at.0, sub_at.1, false));
                    }
                }
            }

            for y in 0..9 {
                if y != ident.1 {
                    for sub in 0..pop {
                        let sub_at = sudoku_rev_map.get(&(ident.0, y, ident.2, sub)).unwrap();
                        coll.push((sub_at.0, sub_at.1, false));
                    }
                }
            }

            let bx = (ident.0 / 3) * 3;
            let by = (ident.1 / 3) * 3;

            for x in bx..(bx + 3) {
                for y in by..(by + 3) {
                    if x != ident.0 && y != ident.1 {
                        for sub in 0..pop {
                            let sub_at = sudoku_rev_map.get(&(x, y, ident.2, sub)).unwrap();
                            coll.push((sub_at.0, sub_at.1, false));
                        }
                    }
                }
            }

            coll.sort();

            cores[at.0].neurons[at.1].neigh.reserve(coll.len());
            for (c, n, ext) in coll.iter() {
                let w = if *ext { 2.5f32 } else { -1.2f32 / 36f32 };
                cores[at.0].neurons[at.1].neigh.push(Neigh {
                    core: *c as u16,
                    neuron: *n as u16,
                    weight: w,
                });
            }
        }
    }

    if let Some(ref probability) = args.mnist {
        let mut all = Vec::new();
        for c in 0..args.core_cnt {
            for n in 0..core_nn_cnt[c] {
                all.push((c, n));
                cores[c].neurons.push(Neuron {
                    state: 0f32,
                    input: 0f32,
                    inj: 0f32,
                    neigh: Vec::new(),
                });
            }
        }

        let targets: HashSet<_> = all.choose_multiple(&mut rng, 10).collect();
        for c in 0..args.core_cnt {
            for n in 0..core_nn_cnt[c] {
                if targets.contains(&(c, n)) {
                    continue;
                }
                for t in targets.iter() {
                    cores[c].neurons[n].neigh.push(Neigh {
                        core: t.0 as u16,
                        neuron: t.1 as u16,
                        weight: 0f32,
                    })
                }
                if rng.gen_bool(*probability) {
                    cores[c].neurons[n].input = 1000000000f32;
                }
            }
        }
    }

    if let Some(conns) = nest_conns {
        for conn in conns.iter() {
            let (sc, sid) = if let Some(i) = nest_rev_map.get(&conn.s) {
                i
            } else {
                continue;
            };
            let (tc, tid) = if let Some(i) = nest_rev_map.get(&conn.t) {
                i
            } else {
                continue;
            };
            let neuron = &mut cores[*sc].neurons[*sid];
            neuron.neigh.push(Neigh {
                core: *tc as u16,
                neuron: *tid as u16,
                weight: conn.w,
            });
            max_syn_per_neuron = max_syn_per_neuron.max(neuron.neigh.len());
        }
    }

    println!("Gen done, max syn per neuron = {}", max_syn_per_neuron);

    let mut final_round_spike_inputs: Vec<Vec<f32>> = core_nn_cnt
        .iter()
        .map(|&nn_count| vec![0.0; nn_count])
        .collect();

    // Simulate
    for i in 0..args.pre_simulate {
        println!("Round {}...", i);

        if i == args.pre_simulate - 1 {
            if let Some(ref p) = args.dump {
                let dram_base = u32::from_str_radix(
                    args.dram_base.trim_start_matches("0x").trim_start_matches("0X"),
                    16,
                ).expect("Invalid --dram-base hex value");
                dump(p, &cores, dram_base, args.num_mc)?;
            }

            if let Some(ref p) = args.dump_genn {
                dump_genn(p, &cores)?;
            }
        }

        let mut fired = 0;
        // Add input
        for c in cores.iter_mut() {
            for n in c.neurons.iter_mut() {
                n.state += n.input;
                n.input = n.inj
            }
        }

        for c in 0..args.core_cnt {
            print!(".");
            for n in 0..core_nn_cnt[c] {
                if cores[c].neurons[n].state > args.threshold {
                    cores[c].neurons[n].state = 0f32;
                    fired += 1;

                    for neigh in 0..cores[c].neurons[n].neigh.len() {
                        let neigh = cores[c].neurons[n].neigh[neigh].clone();
                        cores[neigh.core as usize].neurons[neigh.neuron as usize].input +=
                            neigh.weight;
                        if i == args.pre_simulate - 1 {
                            final_round_spike_inputs[neigh.core as usize]
                                [neigh.neuron as usize] += neigh.weight;
                        }
                    }
                } else {
                    cores[c].neurons[n].state *= e_neg_tau;
                }
            }
        }

        println!(
            "\nRound {}, firing rate {} ({})",
            i,
            fired as f64 / args.tot_neuron as f64,
            fired
        );
    }

    // After all rounds: compute the payload-visible XOR checksum. The payload multiplies
    // by 1.0e6f in FP32 and then converts to int with RNE, so emulate the same path here.
    // The PU's init clears input to 0, so the final hardware input is the spike-only
    // accumulation from the last simulated round.
    if let Some(ref p) = args.dump {
        let mut expected_xor: u32 = 0;
        for (core_idx, c) in cores.iter().enumerate() {
            for (neuron_idx, n) in c.neurons.iter().enumerate() {
                expected_xor ^= quantize_xor_word(n.state);
                expected_xor ^= quantize_xor_word(final_round_spike_inputs[core_idx][neuron_idx]);
            }
        }
        println!("Expected XOR: 0x{:08x}", expected_xor);

        // Patch word 4089 (offset 0x3FE4 from SPM base) in each PU's SPM block
        let spm_size: u32 = 16384;
        let desc_size = (args.core_cnt as u32) * 8;
        let xor_word_offset = (spm_size / 4 - 7) as u64; // word 4089
        let mut dram_file = p.clone();
        dram_file.push("dram.0");
        let mut file = std::fs::OpenOptions::new().write(true).open(&dram_file)?;
        use std::io::Seek;
        for ci in 0..args.core_cnt {
            let spm_base_offset = desc_size as u64 + (ci as u64) * spm_size as u64;
            let byte_offset = spm_base_offset + xor_word_offset * 4;
            file.seek(std::io::SeekFrom::Start(byte_offset))?;
            file.write_all(&expected_xor.to_le_bytes())?;
        }
    }

    Ok(())
}
