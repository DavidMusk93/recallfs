use std::collections::BTreeMap;
use std::env;
use std::hint::black_box;
use std::time::{Duration, Instant};

use hashing::{BackendId, Jump, Maglev, Modulo, Rendezvous, Ring, Selector, hash_key};

#[derive(Clone, Copy)]
struct Config {
    keys: u64,
    lookup_keys: u64,
    rounds: usize,
    backends: u32,
    table_size: usize,
    virtual_nodes: u32,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            keys: 1_000_000,
            lookup_keys: 500_000,
            rounds: 5,
            backends: 32,
            table_size: 4099,
            virtual_nodes: 256,
        }
    }
}

enum Algorithm {
    Modulo(Modulo),
    Ring(Ring),
    Rendezvous(Rendezvous),
    Jump(Jump),
    Maglev(Maglev),
}

impl Algorithm {
    fn build(name: &str, backends: &[BackendId], config: Config) -> Self {
        match name {
            "modulo" => Self::Modulo(Modulo::new(backends)),
            "ring" => Self::Ring(Ring::new(backends, config.virtual_nodes)),
            "rendezvous" => Self::Rendezvous(Rendezvous::new(backends)),
            "jump" => Self::Jump(Jump::new(backends)),
            "maglev" => Self::Maglev(Maglev::new(backends, config.table_size)),
            _ => unreachable!("fixed algorithm name"),
        }
    }
}

impl Selector for Algorithm {
    fn select(&self, key_hash: u64) -> BackendId {
        match self {
            Self::Modulo(selector) => selector.select(key_hash),
            Self::Ring(selector) => selector.select(key_hash),
            Self::Rendezvous(selector) => selector.select(key_hash),
            Self::Jump(selector) => selector.select(key_hash),
            Self::Maglev(selector) => selector.select(key_hash),
        }
    }

    fn memory_bytes(&self) -> usize {
        match self {
            Self::Modulo(selector) => selector.memory_bytes(),
            Self::Ring(selector) => selector.memory_bytes(),
            Self::Rendezvous(selector) => selector.memory_bytes(),
            Self::Jump(selector) => selector.memory_bytes(),
            Self::Maglev(selector) => selector.memory_bytes(),
        }
    }
}

struct Distribution {
    max_over_average: f64,
    min_over_average: f64,
    coefficient_of_variation: f64,
}

fn distribution(selector: &impl Selector, backends: &[BackendId], keys: u64) -> Distribution {
    let mut counts: BTreeMap<BackendId, u64> =
        backends.iter().map(|backend| (*backend, 0)).collect();
    for key in 0..keys {
        *counts
            .get_mut(&selector.select(hash_key(key)))
            .expect("selector returned an unknown backend") += 1;
    }

    let average = keys as f64 / backends.len() as f64;
    let min = *counts.values().min().expect("non-empty backend set") as f64;
    let max = *counts.values().max().expect("non-empty backend set") as f64;
    let variance = counts
        .values()
        .map(|count| {
            let delta = *count as f64 - average;
            delta * delta
        })
        .sum::<f64>()
        / backends.len() as f64;

    Distribution {
        max_over_average: max / average,
        min_over_average: min / average,
        coefficient_of_variation: variance.sqrt() / average,
    }
}

fn churn(before: &impl Selector, after: &impl Selector, keys: u64) -> f64 {
    let changed = (0..keys)
        .filter(|key| {
            let hash = hash_key(*key);
            before.select(hash) != after.select(hash)
        })
        .count();
    changed as f64 / keys as f64
}

fn median_duration(mut values: Vec<Duration>) -> Duration {
    values.sort_unstable();
    values[values.len() / 2]
}

fn lookup_time(selector: &impl Selector, key_hashes: &[u64], rounds: usize) -> (f64, u64) {
    let mut durations = Vec::with_capacity(rounds);
    let mut final_checksum = 0_u64;
    for _ in 0..rounds {
        let start = Instant::now();
        let mut checksum = 0_u64;
        for key_hash in key_hashes {
            checksum = checksum.wrapping_add(selector.select(black_box(*key_hash)) as u64);
        }
        durations.push(start.elapsed());
        final_checksum ^= black_box(checksum);
    }
    let median = median_duration(durations);
    (
        median.as_secs_f64() * 1e9 / key_hashes.len() as f64,
        final_checksum,
    )
}

fn build_time(name: &str, backends: &[BackendId], config: Config) -> f64 {
    let iterations = config.rounds.max(5);
    let mut durations = Vec::with_capacity(iterations);
    let mut bytes = 0_usize;
    for _ in 0..iterations {
        let start = Instant::now();
        let selector = Algorithm::build(name, backends, config);
        durations.push(start.elapsed());
        bytes ^= black_box(selector.memory_bytes());
    }
    black_box(bytes);
    median_duration(durations).as_secs_f64() * 1e6
}

fn parse_config() -> Config {
    let mut config = Config::default();
    let args: Vec<String> = env::args().skip(1).collect();
    let mut index = 0;
    while index < args.len() {
        let value = args
            .get(index + 1)
            .unwrap_or_else(|| panic!("{} requires a value", args[index]));
        match args[index].as_str() {
            "--keys" => config.keys = value.parse().expect("invalid --keys"),
            "--lookup-keys" => config.lookup_keys = value.parse().expect("invalid --lookup-keys"),
            "--rounds" => config.rounds = value.parse().expect("invalid --rounds"),
            "--backends" => config.backends = value.parse().expect("invalid --backends"),
            "--table-size" => config.table_size = value.parse().expect("invalid --table-size"),
            "--vnodes" => config.virtual_nodes = value.parse().expect("invalid --vnodes"),
            unknown => panic!("unknown argument: {unknown}"),
        }
        index += 2;
    }
    assert!(config.keys > 0);
    assert!(config.lookup_keys > 0);
    assert!(config.rounds > 0);
    assert!(config.backends >= 2);
    config
}

fn main() {
    let config = parse_config();
    let base: Vec<BackendId> = (0..config.backends).collect();
    let mut added = base.clone();
    added.push(config.backends);
    let removed_id = config.backends / 2;
    let removed: Vec<_> = base
        .iter()
        .copied()
        .filter(|backend| *backend != removed_id)
        .collect();
    let key_hashes: Vec<_> = (0..config.lookup_keys).map(hash_key).collect();

    println!(
        "# keys={} lookup_keys={} rounds={} backends={} table_size={} vnodes={} removed_id={}",
        config.keys,
        config.lookup_keys,
        config.rounds,
        config.backends,
        config.table_size,
        config.virtual_nodes,
        removed_id
    );
    println!(
        "algorithm,max_over_avg,min_over_avg,cv,add_churn,remove_middle_churn,lookup_ns,build_us,memory_bytes,checksum"
    );

    for name in ["modulo", "ring", "rendezvous", "jump", "maglev"] {
        let selector = Algorithm::build(name, &base, config);
        let after_add = Algorithm::build(name, &added, config);
        let after_remove = Algorithm::build(name, &removed, config);
        let spread = distribution(&selector, &base, config.keys);
        let add_churn = churn(&selector, &after_add, config.keys);
        let remove_churn = churn(&selector, &after_remove, config.keys);
        let (lookup_ns, checksum) = lookup_time(&selector, &key_hashes, config.rounds);
        let build_us = build_time(name, &base, config);

        println!(
            "{name},{:.6},{:.6},{:.6},{:.6},{:.6},{:.3},{:.3},{},{}",
            spread.max_over_average,
            spread.min_over_average,
            spread.coefficient_of_variation,
            add_churn,
            remove_churn,
            lookup_ns,
            build_us,
            selector.memory_bytes(),
            checksum
        );
    }
}
