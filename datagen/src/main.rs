use std::{collections::HashMap, fs::File, path::PathBuf};

use byteorder::ByteOrder;
use byteorder::LittleEndian;
use byteorder::WriteBytesExt;
use clap::Parser;
use rand::seq::SliceRandom;
use rand::{SeedableRng, Rng};
use rand_distr::{Geometric, Distribution, Uniform};
use rand_xoshiro::Xoshiro256PlusPlus;
use std::io::Write;
use std::io::BufReader;
use std::io::BufWriter;
use serde::Deserialize;

#[derive(Parser)]
struct Args {
    #[clap(short, long)]
    core_cnt: usize,

    #[clap(long)]
    load_nest_nodes: Option<PathBuf>,

    #[clap(long)]
    load_nest_conns: Option<PathBuf>,

    #[clap(long)]
    nest_randomize: bool,

    // #[clap(short, long, default_value="1000")]
    // neuron_per_core: usize,
    #[clap(short, long, default_value="82601")]
    tot_neuron: usize,

    #[clap(long, default_value="0.081167")]
    connectivity: f64,

    #[clap(short, long, default_value="100")]
    pre_simulate: usize,

    #[clap(long, default_value="0.1")]
    tau: f32,

    #[clap(long, default_value="15")]
    threshold: f32,

    #[clap(long, default_value="19260817")]
    seed: u64,

    #[clap(long, default_value="0.273786")]
    inh_ratio: f64,

    #[clap(short, long)]
    dump: Option<PathBuf>,

    #[clap(long)]
    dump_genn: Option<PathBuf>,
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
    neurons: Vec<Neuron>
}

fn dump(base: &PathBuf, cores: &Vec<Core>) -> anyhow::Result<()> {
    println!("Dumping to {}", base.display());
    // Dump
    let mut dram = Vec::new();
    // TODO: dram reserve

    for ci in 0..cores.len() {
        let c = &cores[ci];
        let mut spm = Vec::new();
        for n in c.neurons.iter() {
            let neigh_start = dram.len() * 4;
            for neigh in n.neigh.iter() {
                let col = ((neigh.core as u32) << 16) | (neigh.neuron as u32);
                dram.push(col);
                dram.push(neigh.weight.to_bits());
            }
            let neigh_end = dram.len() * 4;
            spm.push(n.state.to_bits());
            spm.push(n.input.to_bits());
            spm.push(neigh_start as u32);
            spm.push(neigh_end as u32);
        }

        let mut spm_file = base.clone();
        spm_file.push(format!("spm.{}.bin", ci));
        spm.resize(16384 / 4, 0);
        spm[16384 / 4 - 1] = c.neurons.len() as u32;

        let spm_slice: &[u8] = unsafe {
            core::slice::from_raw_parts(spm.as_slice().as_ptr() as *const u8, spm.len() * 4)
        };
        std::fs::write(spm_file, spm_slice)?;
    }

    let mut dram_file = base.clone();
    dram_file.push("dram.bin");
    let dram_slice: &[u8] = unsafe {
        core::slice::from_raw_parts(dram.as_slice().as_ptr() as *const u8, dram.len() * 4)
    };
    std::fs::write(dram_file, dram_slice)?;

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
                output.write_u32::<LittleEndian>(prefix_sum[neigh.core as usize] as u32 + neigh.neuron as u32)?;
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

fn main() -> anyhow::Result<()> {
    let mut args = Args::parse();

    let mut rng = Xoshiro256PlusPlus::seed_from_u64(args.seed);

    let e_neg_tau = std::f32::consts::E.powf(-args.tau);

    let neigh_dist = Geometric::new(args.connectivity)?;
    let ext_weight_dist = Uniform::new(0.85, 0.92);
    let inh_weight_dist = Uniform::new(-2.5f32, -2.44);
    let inj_weight_dist = Uniform::new(0.3, 0.8);
    let init_state_dist = Uniform::new(0f32, args.threshold);

    let mut nest_nodes: Option<Vec<NestNode>> = args.load_nest_nodes.as_ref().map(|p| serde_json::from_reader(BufReader::new(std::fs::File::open(p).unwrap())).unwrap());
    let nest_conns: Option<Vec<NestConn>> = args.load_nest_conns.as_ref().map(|p| serde_json::from_reader(BufReader::new(std::fs::File::open(p).unwrap())).unwrap());

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

            let INIT_FIRING_RATE = 0.1;
            let fired = rng.gen_bool(INIT_FIRING_RATE);

            cores[core].neurons.push(Neuron {
                state,
                input: if fired { 1000000000f32 } else { 0f32 },
                inj: 0f32,
                neigh: Vec::new(),
            })
        }
      } else {
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

              let w = if is_inh { inh_weight_dist } else { ext_weight_dist } .sample(&mut rng);
              neigh.push(Neigh {
                  core: cur_core as u16,
                  neuron: cur as u16,
                  weight: w as f32
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

    if let Some(conns) = nest_conns {
        for conn in conns.iter() {
            let (sc, sid) = if let Some(i) = nest_rev_map.get(&conn.s) { i } else { continue };
            let (tc, tid) = if let Some(i) = nest_rev_map.get(&conn.t) { i } else { continue };
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

    // Simulate 
    for i in 0..args.pre_simulate {
        println!("Round {}...", i);

        if i == args.pre_simulate - 1 {
            if let Some(ref p) = args.dump {
                dump(p, &cores)?;
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
                        cores[neigh.core as usize].neurons[neigh.neuron as usize].input += neigh.weight;
                    }
                } else {
                    cores[c].neurons[n].state *= e_neg_tau;
                }
            }
        }

        println!("\nRound {}, firing rate {} ({})", i, fired as f64 / args.tot_neuron as f64, fired);
    }

    Ok(())
}
