---
doc_id: recallfs-source-ringzero-study-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-source-ringzero-snapshot-v1
supersedes: []
verified_by:
  - source digest review
---

# Sources

Access date for every web source: 2026-09-17.

## Primary Source

| Field | Value |
| --- | --- |
| Project | RingZero |
| URL | <https://github.com/immanuwell/ringzero> |
| Revision | `9a56125f02fcbce55448482d442d99729573ab06` |
| Local archive | [`../../sources/20260917-ringzero.md`](../../sources/20260917-ringzero.md) |
| Source status | Complete git tree inspected; source not vendored because no license is granted |

## Papers

| Role | Source | Local archive | SHA-256 |
| --- | --- | --- | --- |
| XDP design and evaluation | Toke Høiland-Jørgensen et al., *The eXpress Data Path*, CoNEXT 2018, DOI `10.1145/3281411.3281443` | [`../../sources/20260917-xdp-conext18.pdf`](../../sources/20260917-xdp-conext18.pdf) | `445cd34b8898a465eb006d91f5b3845c50a21a9fdc53c1c9ad68fee7b6c0b2cb` |
| Maglev system and hashing | Eisenbud et al., *Maglev: A Fast and Reliable Software Network Load Balancer*, NSDI 2016 | [`../../sources/20260917-maglev-nsdi16.pdf`](../../sources/20260917-maglev-nsdi16.pdf) | `4eb261dfb9fe1b872318740fb92f582be388141010df9cbfc71e2ca65e9c7367` |

## Kernel Documentation

| Topic | URL | Used for |
| --- | --- | --- |
| AF_XDP | <https://docs.kernel.org/networking/af_xdp.html> | UMEM ownership, RX/TX/FILL/COMPLETION rings, copy versus zero-copy modes, queue binding |
| XDP RX metadata | <https://docs.kernel.org/networking/xdp-rx-metadata.html> | Driver-dependent metadata and redirect boundaries |
| BPF program run | <https://docs.kernel.org/bpf/bpf_prog_run.html> | Difference between isolated `BPF_PROG_RUN` and real redirect/transmit behavior |
| mlx5 counters | <https://docs.kernel.org/networking/device_drivers/ethernet/mellanox/mlx5/counters.html> | Target-machine XDP drop/redirect/transmit evidence |

## Related Implementations

| Source | URL | Relevance |
| --- | --- | --- |
| Katran | <https://github.com/facebookincubator/katran> | Production-oriented XDP L4 load balancer that combines connection tracking, encapsulation, weighted Maglev, health/config management, and operational tooling |
| xdp-tools `xdp-bench` | <https://github.com/xdp-project/xdp-tools> | Baseline XDP DROP, PASS, TX, redirect, and AF_XDP measurement |

## Evidence Classification

- RingZero source observations apply only to revision
  `9a56125f02fcbce55448482d442d99729573ab06`.
- CoNEXT'18 and NSDI'16 numbers are author-reported historical results, not
  measurements reproduced by this study.
- The maintained C benchmark isolates selector behavior; it is not an XDP,
  eBPF, NIC, or end-to-end load-balancer benchmark.
- d2 evidence covers verifier acceptance, native XDP on veth and one bounded
  10,000-packet sequence-ledger run. It does not cover a physical NIC,
  RSS/multi-queue scaling, sustained high load or a line-rate zero-loss
  envelope.
