# Changelog

## 0.1.0 — Bootstrap I — gcc 4.8 fixed point (2026-05-06)

First tagged release. Sea-front clears the canonical bootstrappable.org
rung: cc1plus-built-by-sea-front from gcc 4.8 source is a self-consistent
fixed point under the gcc bootstrap protocol.

### Highlights

- **stage2 == stage3 byte-equal.** Rebuilding cc1plus through itself a
  second time produces a bit-identical binary modulo BUILD-ID, DWARF
  debug info, and gcc's own embedded `executable_checksum` (a 16-byte
  MD5 over input .o files whose debug-info timestamps differ between
  stages — non-determinism in gcc 4.8 itself, not in sea-front).
  18,601,376 bytes of meaningful content match exactly.
- **Working stage-1 cc1plus binary.** Built end-to-end via sea-front
  from gcc 4.8's C++ source. Compiles and runs real C++ programs.
- **Era-correct glibc shim** (`scripts/glibc-shim/`) replaces the
  earlier compat-header hacks. Surgical overrides: glibc 2.17 cdefs.h
  with empty stubs for post-2.17 attribute decorators; bits/floatn.h
  + bits/floatn-common.h with `__HAVE_FLOAT*=0` so modern math.h
  skips _FloatN declarations.
- **`scripts/cc1plus-via-sf`** — drives a sea-front-built cc1plus as
  a g++ replacement, used for both stage-N rebuilds and g++.dg testing.

### Test counts

- 144 lexer unit tests
- 42 parser integration tests
- 301 emit-c end-to-end tests (compile → execute → verify)
- 4 multi-TU dedup tests
- 28 gated + 52 stretch libstdc++ header smoke tests

### Bootstrap chain

```
cc1plus-stage0  53.3 MB  (built by sea-front-cc — initial host bootstrap)
cc1plus-stage1  24.9 MB  (built by stage-0 via cc1plus-via-sf)
cc1plus-stage2  23.7 MB  (built by stage-1 — first fully self-built)
cc1plus-stage3  23.7 MB  (built by stage-2 — fixed point)
```

### Known gaps

- C++11+ features incrementally supported; current target is C++03 for
  the gcc 4.8 bootstrap.
- Lambdas, partial template specialization, SFINAE, variadic templates
  not yet implemented.
- One known cc1plus segfault on insn-recog.c::recog_63 affects stage-0
  only; stage-1 and beyond compile it correctly. Tracked separately.
