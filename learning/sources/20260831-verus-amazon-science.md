# Amazon Science Verus Article Archive

| Field | Value |
| --- | --- |
| URL | `https://www.amazon.science/blog/developing-provably-correct-rust-code-with-verus` |
| Title | Developing provably correct Rust code with Verus |
| Author | Bryan Parno |
| Published | 2026-08-31 |
| Access Date | 2026-09-07 |

## 1. Archived Summary

The article introduces Verus as an open-source automated program verifier for
Rust. Developers write mathematical specifications and proof guidance next to
executable Rust code. Verus then checks, statically, whether the implementation
satisfies those specifications for all inputs covered by the preconditions.

The article positions Verus above Rust's existing safety guarantees. Rust can
prevent broad classes of memory-safety and concurrency errors, but it does not
prove that an algorithm returns the intended answer, never reaches an invalid
index, preserves a data-structure invariant, or avoids leaking information.
Verus targets these functional-correctness properties.

## 2. Main Claims

| Topic | Archived Claim |
| --- | --- |
| Verification scope | Verus checks an implementation against a formal specification for all possible inputs under the specification's assumptions. |
| Source integration | Specifications and proofs use Rust-like syntax in the Rust source rather than a separate proof language. |
| Automation | SMT solvers discharge many low-level proof obligations; developers provide contracts, invariants, and high-level proof structure. |
| Feedback | The article reports typical feedback in under a second and IDE integration suitable for an interactive loop. |
| Runtime cost | Specification and proof code is erased before ordinary compilation. |
| Unsafe Rust | Verus can be used to prove safety obligations for selected unsafe implementations. |
| Concurrency | Lock invariants and custom locking schemes can be modeled and proved. |
| Amazon usage | Amazon reports verifying key primitives used by the Nitro Isolation Engine and other critical infrastructure. |

## 3. Examples Named by the Article

| Project | Property Area |
| --- | --- |
| Vest | Correct and secure binary format parsers and serializers |
| Verdict | X.509 certificate validation with user-supplied policies |
| CapybaraKV | Persistent-memory log correctness and crash safety |
| Atmosphere | Microkernel correctness |
| Anvil | Kubernetes controller safety and liveness |
| CortenMM | Concurrent memory-management and locking protocols |

## 4. Important Qualification

The article explicitly states that Verus guarantees depend on:

- the correctness and completeness of the top-level specification,
- bottom-level assumptions about libraries and the runtime,
- the Verus verifier and its solver stack,
- the Rust compiler toolchain that produces the executable.

The claim is therefore not "the program is absolutely correct." It is "the
implementation satisfies the stated specification, under the modeled
assumptions and trusted toolchain."
