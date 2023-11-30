use std::{path::PathBuf, fs::File};

use clap::Parser;
use rand::SeedableRng;
use rand_distr::{Geometric, Distribution, Uniform};
use rand_xoshiro::Xoshiro256PlusPlus;
use std::io::Write;

#[derive(Parser)]
struct Args {
    #[clap(short, long)]
    core_cnt: usize,

    #[clap(short, long, default_value="1000")]
    neuron_per_core: usize,

    #[clap(long, default_value="0.01")]
    connectivity: f64,

    #[clap(short, long, default_value="100")]
    pre_simulate: usize,

    #[clap(long, default_value="1")]
    injective: f32,

    #[clap(long, default_value="0.1")]
    tau: f32,

    #[clap(long, default_value="10")]
    threshold: f32,

    #[clap(long, default_value="19260817")]
    seed: u64,

    #[clap(long, default_value="0.2")]
    max_weight: f32,

    #[clap(long, default_value="-0.1")]
    min_weight: f32,

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
    neigh: Vec<Neigh>
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

    let mut output = File::create(base)?;

    let mut prefix_sum = Vec::with_capacity(cores.len());
    let mut prefix_sum_cur = 0;
    for c in cores.iter() {
        prefix_sum.push(prefix_sum_cur);
        prefix_sum_cur += c.neurons.len();
    }

    writeln!(output, "{}", prefix_sum_cur)?;
    for c in cores.iter() {
        for n in c.neurons.iter() {
            writeln!(output, "{}", n.state)?;
        }
    }

    // Actually CSR
    let mut tot_syn = 0;
    for c in cores.iter() {
        for n in c.neurons.iter() {
            write!(output, "{} ", tot_syn)?;
            tot_syn += n.neigh.len();
        }
    }
    writeln!(output, "{}", tot_syn)?;
    for c in cores.iter() {
        for n in c.neurons.iter() {
            for neigh in n.neigh.iter() {
                writeln!(output, "{} {}", prefix_sum[neigh.core as usize] + neigh.neuron as usize, neigh.weight)?;
            }
        }
    }

    Ok(())
}

fn main() -> anyhow::Result<()> {
    let args = Args::parse();

    let mut rng = Xoshiro256PlusPlus::seed_from_u64(args.seed);

    let e_neg_tau = std::f32::consts::E.powf(-args.tau);

    let nn = args.core_cnt * args.neuron_per_core;
    let neigh_dist = Geometric::new(args.connectivity)?;
    let weight_dist = Uniform::new(args.min_weight, args.max_weight);
    let init_state_dist = Uniform::new(0f32, args.threshold);

    // Generate
    let mut cores: Vec<Core> = vec![Default::default(); args.core_cnt];
    let mut max_syn_per_neuron = 0;
    for core in 0..args.core_cnt {
        println!("Gen: core {}", core);
        cores[core].neurons.reserve(args.neuron_per_core);
        for _ in 0..args.neuron_per_core {
            let mut neigh = Vec::new();
            let mut cur = 0u64;

            loop {
                cur += neigh_dist.sample(&mut rng);
                if cur >= nn as u64 { break; }
                neigh.push(Neigh {
                    core: (cur / args.neuron_per_core as u64) as u16,
                    neuron: (cur % args.neuron_per_core as u64) as u16,
                    weight: weight_dist.sample(&mut rng),
                })
            }

            max_syn_per_neuron = max_syn_per_neuron.max(neigh.len());

            cores[core].neurons.push(Neuron {
                state: init_state_dist.sample(&mut rng),
                input: args.injective,
                neigh,
            })
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
                n.input = args.injective;
            }
        }

        for c in 0..args.core_cnt {
            print!(".");
            for n in 0..args.neuron_per_core {
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

        println!("\nRound {}, firing rate {} ({})", i, fired as f64 / nn as f64, fired);
    }

    Ok(())
}