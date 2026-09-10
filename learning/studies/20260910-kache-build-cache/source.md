---
doc_id: recallfs-study-kache-build-cache-sources-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-kache-build-cache
depends_on: []
supersedes: []
verified_by:
  - upstream revisions and SHA-256 digests below
---

# Sources

Access date: 2026-09-10.

## Kache

| Field | Value |
| --- | --- |
| Repository | <https://github.com/kunobi-ninja/kache> |
| Release | `v0.19.0` |
| Commit | `89b0c738115534156e759f157d0ce681b282d484` |
| Commit date | `2026-09-09T19:05:46+02:00` |
| License | Apache-2.0 |
| Archived README | [`learning/sources/20260910-kache-v0.19.0-README.md`](../../sources/20260910-kache-v0.19.0-README.md) |
| Archived README SHA-256 | `13064017a6fe40d8125337929bc6b6d8740269fc21f2e53442114fb07be1d267` |

Primary implementation and documentation inspected at the pinned commit:

- [architecture.mdx](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/docs/how-it-works/architecture.mdx)
- [cache-key.mdx](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/docs/how-it-works/cache-key.mdx)
- [c-cpp.mdx](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/docs/getting-started/c-cpp.mdx)
- [ci.mdx](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/docs/remote-cache/ci.mdx)
- [configuration.mdx](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/docs/getting-started/configuration.mdx)
- [compiler/cc.rs](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/src/compiler/cc.rs)
- [cache_key.rs](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/src/cache_key.rs)
- [wrapper.rs](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/src/wrapper.rs)
- [policy.rs](https://github.com/kunobi-ninja/kache/blob/89b0c738115534156e759f157d0ce681b282d484/src/policy.rs)

Relevant source digests:

| File | SHA-256 |
| --- | --- |
| `docs/how-it-works/architecture.mdx` | `d8579f5dde8e65d91352042dc135a164abe1edd13124079060a864de66117ae8` |
| `docs/how-it-works/cache-key.mdx` | `210c96659732a76867001c86c243a66027783ea20d7b248dfe543ded8f5e2a87` |
| `docs/getting-started/c-cpp.mdx` | `7ae1571e015a1c12c32aae1e1c2bf8707dd2ba3f864f9893fd3c2c0b4576a3ed` |
| `docs/remote-cache/ci.mdx` | `8715ea7e69a5d27c4443bfbb94786c8d217a23385ef2b6a3dab82ffadcbad9e3` |

The release binary used for local reproduction was downloaded from:

`https://github.com/kunobi-ninja/kache/releases/download/v0.19.0/kache-aarch64-apple-darwin.tar.gz`

| Artifact | SHA-256 |
| --- | --- |
| Release archive | `8dcdaa95f3678b00696ca4742d8ad67c1f00d9baa7d142cbf9337b3f3ecb03b7` |
| Extracted `kache` binary | `c08b2d223df93f65454109a89f875f79c0ea5f73beb802b67a0b57e664dafec9` |

## Blade Compatibility Reference

The public Blade repository was inspected to test the integration boundary:

| Field | Value |
| --- | --- |
| Repository | <https://github.com/blade-build/blade-build> |
| Commit | `b95bff3e4e35c3b53265c7522e42e1aedf84f3f8` |
| Commit date | `2026-08-13T22:17:42+08:00` |

Relevant files:

- [build_accelerator.py](https://github.com/blade-build/blade-build/blob/b95bff3e4e35c3b53265c7522e42e1aedf84f3f8/src/blade/build_accelerator.py)
- [cc_rule_support.py](https://github.com/blade-build/blade-build/blob/b95bff3e4e35c3b53265c7522e42e1aedf84f3f8/src/blade/cc_rule_support.py)
- [toolchain.py](https://github.com/blade-build/blade-build/blob/b95bff3e4e35c3b53265c7522e42e1aedf84f3f8/src/blade/toolchain.py)
- [C/C++ toolchain configuration](https://github.com/blade-build/blade-build/blob/b95bff3e4e35c3b53265c7522e42e1aedf84f3f8/doc/en/config.md)

This public repository is only a compatibility reference. Internal Blade
forks with Sailfish, Goma, or CAS are separate systems and must be probed on
the actual Jenkins runner.

## Access Notes

The supplied GitHub page timed out in the IDE browser. The public page and
README were retrieved through HTTP, and both repositories were then cloned.
All implementation claims in the study are tied to the revisions above.
