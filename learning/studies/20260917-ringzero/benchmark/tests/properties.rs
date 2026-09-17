use std::collections::{BTreeMap, BTreeSet};

use hashing::{
    Maglev, Modulo, Selector, build_maglev_table_from_parameters, build_maglev_table_in_order,
    hash_key,
};

#[test]
fn maglev_matches_the_nsdi16_worked_example() {
    let backends = [0, 1, 2];
    let parameters = [(3, 4), (0, 2), (3, 1)];
    let table = build_maglev_table_from_parameters(&backends, &parameters, 7);
    assert_eq!(table, [1, 0, 1, 0, 2, 2, 0]);

    let after_removal = build_maglev_table_from_parameters(&[0, 2], &[(3, 4), (3, 1)], 7);
    assert_eq!(after_removal, [0, 0, 0, 0, 2, 2, 2]);
}

#[test]
fn canonical_maglev_is_independent_of_input_order() {
    let forward = Maglev::new(&[1, 2, 3, 4], 4099);
    let reversed = Maglev::new(&[4, 3, 2, 1], 4099);
    assert_eq!(forward.table(), reversed.table());
}

#[test]
fn raw_backend_iteration_order_changes_the_maglev_table() {
    let forward = build_maglev_table_in_order(&[1, 2, 3, 4], 4099);
    let reversed = build_maglev_table_in_order(&[4, 3, 2, 1], 4099);
    let changed = forward
        .iter()
        .zip(&reversed)
        .filter(|(left, right)| left != right)
        .count();
    assert_eq!(changed, 8);
}

#[test]
fn maglev_assigns_each_backend_either_floor_or_ceil_slots() {
    let backends: Vec<_> = (0..32).collect();
    let maglev = Maglev::new(&backends, 4099);
    let mut counts: BTreeMap<_, usize> = backends.iter().map(|backend| (*backend, 0)).collect();
    for backend in maglev.table() {
        *counts.get_mut(backend).expect("unknown backend") += 1;
    }
    let minimum = *counts.values().min().unwrap();
    let maximum = *counts.values().max().unwrap();
    assert_eq!(maximum - minimum, 1);
    assert_eq!(counts.values().sum::<usize>(), 4099);
}

#[test]
fn maglev_lookup_returns_only_configured_backends() {
    let backends = [7, 11, 42, 99];
    let expected: BTreeSet<_> = backends.into_iter().collect();
    let selector = Maglev::new(&backends, 4099);
    let observed: BTreeSet<_> = (0..100_000)
        .map(|key| selector.select(hash_key(key)))
        .collect();
    assert_eq!(observed, expected);
}

#[test]
fn modulo_reassigns_most_keys_when_the_backend_count_changes() {
    let before = Modulo::new(&(0..32).collect::<Vec<_>>());
    let after = Modulo::new(&(0..33).collect::<Vec<_>>());
    let changed = (0..100_000)
        .filter(|key| {
            let hash = hash_key(*key);
            before.select(hash) != after.select(hash)
        })
        .count();
    assert!(changed > 90_000, "changed only {changed} keys");
}

#[test]
fn maglev_addition_has_bounded_but_not_zero_disruption() {
    let before = Maglev::new(&(0..32).collect::<Vec<_>>(), 4099);
    let after = Maglev::new(&(0..33).collect::<Vec<_>>(), 4099);
    let changed = (0..100_000)
        .filter(|key| {
            let hash = hash_key(*key);
            before.select(hash) != after.select(hash)
        })
        .count();
    assert!(
        (2_000..8_000).contains(&changed),
        "unexpected changed-key count: {changed}"
    );
}
