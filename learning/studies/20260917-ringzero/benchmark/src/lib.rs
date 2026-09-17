use std::mem::size_of;

pub type BackendId = u32;

const KEY_SEED: u64 = 0x9e37_79b9_7f4a_7c15;
const NODE_SEED_1: u64 = 0x243f_6a88_85a3_08d3;
const NODE_SEED_2: u64 = 0x1319_8a2e_0370_7344;

pub fn mix64(mut value: u64) -> u64 {
    value ^= value >> 30;
    value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value ^= value >> 27;
    value = value.wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

pub fn hash_key(key: u64) -> u64 {
    mix64(key ^ KEY_SEED)
}

fn canonical_backends(backends: &[BackendId]) -> Vec<BackendId> {
    assert!(!backends.is_empty(), "at least one backend is required");
    let mut result = backends.to_vec();
    result.sort_unstable();
    result.dedup();
    assert_eq!(
        result.len(),
        backends.len(),
        "backend identifiers must be unique"
    );
    result
}

pub trait Selector {
    fn select(&self, key_hash: u64) -> BackendId;
    fn memory_bytes(&self) -> usize;
}

pub struct Modulo {
    backends: Vec<BackendId>,
}

impl Modulo {
    pub fn new(backends: &[BackendId]) -> Self {
        Self {
            backends: canonical_backends(backends),
        }
    }
}

impl Selector for Modulo {
    fn select(&self, key_hash: u64) -> BackendId {
        self.backends[(key_hash % self.backends.len() as u64) as usize]
    }

    fn memory_bytes(&self) -> usize {
        self.backends.len() * size_of::<BackendId>()
    }
}

pub struct Ring {
    points: Vec<(u64, BackendId)>,
}

impl Ring {
    pub fn new(backends: &[BackendId], virtual_nodes: u32) -> Self {
        assert!(virtual_nodes > 0, "virtual node count must be non-zero");
        let backends = canonical_backends(backends);
        let mut points = Vec::with_capacity(backends.len() * virtual_nodes as usize);
        for backend in backends {
            for vnode in 0..virtual_nodes {
                let identity = ((backend as u64) << 32) | vnode as u64;
                points.push((mix64(identity ^ NODE_SEED_1), backend));
            }
        }
        points.sort_unstable();
        Self { points }
    }
}

impl Selector for Ring {
    fn select(&self, key_hash: u64) -> BackendId {
        let index = self.points.partition_point(|(token, _)| *token < key_hash);
        self.points[if index == self.points.len() { 0 } else { index }].1
    }

    fn memory_bytes(&self) -> usize {
        self.points.len() * size_of::<(u64, BackendId)>()
    }
}

pub struct Rendezvous {
    backends: Vec<BackendId>,
}

impl Rendezvous {
    pub fn new(backends: &[BackendId]) -> Self {
        Self {
            backends: canonical_backends(backends),
        }
    }
}

impl Selector for Rendezvous {
    fn select(&self, key_hash: u64) -> BackendId {
        self.backends
            .iter()
            .copied()
            .max_by_key(|backend| {
                let node_hash = mix64(*backend as u64 ^ NODE_SEED_1);
                (mix64(key_hash ^ node_hash), *backend)
            })
            .expect("validated non-empty backend set")
    }

    fn memory_bytes(&self) -> usize {
        self.backends.len() * size_of::<BackendId>()
    }
}

pub struct Jump {
    backends: Vec<BackendId>,
}

impl Jump {
    pub fn new(backends: &[BackendId]) -> Self {
        Self {
            backends: canonical_backends(backends),
        }
    }
}

impl Selector for Jump {
    fn select(&self, key_hash: u64) -> BackendId {
        self.backends[jump_bucket(key_hash, self.backends.len() as i32) as usize]
    }

    fn memory_bytes(&self) -> usize {
        self.backends.len() * size_of::<BackendId>()
    }
}

pub fn jump_bucket(mut key: u64, bucket_count: i32) -> i32 {
    assert!(bucket_count > 0, "bucket count must be positive");
    let mut previous = -1_i64;
    let mut next = 0_i64;
    while next < bucket_count as i64 {
        previous = next;
        key = key.wrapping_mul(2_862_933_555_777_941_757).wrapping_add(1);
        next = ((previous + 1) as f64 * ((1_u64 << 31) as f64 / ((key >> 33) + 1) as f64)) as i64;
    }
    previous as i32
}

pub struct Maglev {
    backends: Vec<BackendId>,
    table: Vec<BackendId>,
}

impl Maglev {
    pub fn new(backends: &[BackendId], table_size: usize) -> Self {
        let backends = canonical_backends(backends);
        let table = build_maglev_table_in_order(&backends, table_size);
        Self { backends, table }
    }

    pub fn table(&self) -> &[BackendId] {
        &self.table
    }
}

impl Selector for Maglev {
    fn select(&self, key_hash: u64) -> BackendId {
        self.table[(key_hash % self.table.len() as u64) as usize]
    }

    fn memory_bytes(&self) -> usize {
        (self.backends.len() + self.table.len()) * size_of::<BackendId>()
    }
}

fn maglev_parameters(backend: BackendId, table_size: usize) -> (usize, usize) {
    let identity = backend as u64;
    let offset = (mix64(identity ^ NODE_SEED_1) % table_size as u64) as usize;
    let skip = (mix64(identity ^ NODE_SEED_2) % (table_size - 1) as u64) as usize + 1;
    (offset, skip)
}

/// Builds the table using exactly the supplied backend order.
///
/// Production callers should use `Maglev::new`, which canonicalizes order.
/// This function is public so the study can demonstrate why raw hash-map
/// iteration order is not a valid distributed configuration contract.
pub fn build_maglev_table_in_order(backends: &[BackendId], table_size: usize) -> Vec<BackendId> {
    assert!(!backends.is_empty(), "at least one backend is required");
    assert!(
        table_size >= backends.len() && is_prime(table_size),
        "Maglev table size must be prime and at least the backend count"
    );

    let parameters: Vec<_> = backends
        .iter()
        .map(|backend| maglev_parameters(*backend, table_size))
        .collect();
    build_maglev_table_from_parameters(backends, &parameters, table_size)
}

pub fn build_maglev_table_from_parameters(
    backends: &[BackendId],
    parameters: &[(usize, usize)],
    table_size: usize,
) -> Vec<BackendId> {
    assert_eq!(backends.len(), parameters.len());
    assert!(!backends.is_empty());
    assert!(table_size >= backends.len());
    for (offset, skip) in parameters {
        assert!(*offset < table_size);
        assert!((1..table_size).contains(skip));
        assert_eq!(greatest_common_divisor(*skip, table_size), 1);
    }
    let mut table = vec![None; table_size];
    let mut next = vec![0_usize; backends.len()];
    let mut filled = 0;

    while filled < table_size {
        for (backend_index, backend) in backends.iter().enumerate() {
            let (offset, skip) = parameters[backend_index];
            let candidate_at = |position: usize| {
                ((offset as u128 + position as u128 * skip as u128) % table_size as u128) as usize
            };
            let mut candidate = candidate_at(next[backend_index]);
            while table[candidate].is_some() {
                next[backend_index] += 1;
                candidate = candidate_at(next[backend_index]);
            }
            table[candidate] = Some(*backend);
            next[backend_index] += 1;
            filled += 1;
            if filled == table_size {
                break;
            }
        }
    }

    table
        .into_iter()
        .map(|entry| entry.expect("all slots are filled"))
        .collect()
}

fn greatest_common_divisor(mut left: usize, mut right: usize) -> usize {
    while right != 0 {
        (left, right) = (right, left % right);
    }
    left
}

pub fn is_prime(value: usize) -> bool {
    if value < 2 {
        return false;
    }
    if value.is_multiple_of(2) {
        return value == 2;
    }
    let mut divisor = 3;
    while divisor <= value / divisor {
        if value.is_multiple_of(divisor) {
            return false;
        }
        divisor += 2;
    }
    true
}
