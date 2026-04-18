# Two-Phase Name Lookup — Implementation Plan

## Why

sea-front currently uses 11 ad-hoc heuristics to handle template-dependent
names. These work for gcc 4.8's C++03 patterns but are not principled:

- 1-char uppercase tag checks (in clone.c, instantiate.c, stmt.c)
- SubstMap-tag-lookup fallback for non-TY_DEPENDENT types
- Sema already_typed override for class members
- Statement disambiguation for dependent qualified names
- Best-effort method dispatch using arg types instead of decl types

All stem from one gap: the parser doesn't reliably track which names
depend on template parameters, and the clone/sema/codegen passes lack
phase-2 lookup to resolve them at instantiation time.

## Standard Reference

N4659 §17.7 [temp.res]:
- Phase 1 (definition time): non-dependent names are looked up and bound
- Phase 2 (instantiation time): dependent names are looked up in the
  instantiation context

## Stages

### Stage 0: Fix Template Parameter Scope Visibility (2-3 days)

Ensure the REGION_TEMPLATE scope is always on the lookup chain when
parsing partial specialization bodies. The root cause: scope chaining
during class body parsing and deferred body replay.

Files: decl.c, type.c, lookup.c

### Stage 1: is_type_dependent Flag Propagation (2-3 days)

Add bool is_type_dependent to Node. Set at parse time when a name
resolves to TY_DEPENDENT; propagate bottom-up through compound
expressions. Replaces 1-char uppercase heuristic in stmt.c.

Files: parse.h, expr.c, stmt.c, type.c

### Stage 2: Phase-2 Lookup in clone.c (3-4 days)

Replace token-swapping in ND_QUALIFIED with actual qualified lookup
in the substituted type's class_region. For ND_IDENT with
needs_phase2_lookup, resolve via the instantiated class region.

Files: clone.c, parse.h

### Stage 3: Instantiation-Time Sema (2 days)

Split visit_ident into definition-time and instantiation-time modes.
Trust phase-2 resolved types from clone. Remove already_typed
workaround.

Files: sema.c, instantiate.c

### Stage 4: Fix Codegen Method Dispatch (1-2 days)

With proper class_region patching, remove template-arg-presence
heuristic and call-site-arg-type mangling fallback.

Files: emit_c.c

### Stage 5: Heuristic Removal (1 day)

Delete all TODO(seafront#dep-scope), TODO(seafront#two-phase),
and the 11 heuristic code blocks.

## Total: 11-15 developer-days

## What's Deferred

- SFINAE / enable_if (not in gcc 4.8 C++03 headers)
- ADL in dependent contexts (gcc 4.8 uses qualified calls)
- Variadic templates
- Full value-dependent expression tracking
- Dependent base class unqualified lookup
