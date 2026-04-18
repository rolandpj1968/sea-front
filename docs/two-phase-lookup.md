# Two-Phase Name Lookup — Implementation Status

## Summary

Implemented Stages 0-4 of the two-phase lookup plan. Heuristic count
reduced from 11 template-related shortcuts to 2 (both with proper
standard justification). The remaining shortcuts are for C++03-specific
patterns (vNULL conversion, arg-type mangling fallback) and will be
addressed when/if we target C++11.

## What Was Done

### Stage 0: Fix Template Parameter Scope Visibility ✓

Root cause found and fixed: OOL method definitions (`template<T,A>
void vec<T,A,vl_ptr>::release()`) set `p->region = qscope` (the
class scope), disconnecting the template scope from the lookup chain.

Fix: create a shallow copy of qscope with `enclosing` chained through
the template scope. The original class scope is untouched (avoids
cycle). N4659 §17.7/4 [temp.res].

### Stage 1: is_type_dependent Flag ✓

Added `bool is_type_dependent` to Node (parse.h). Propagated through
sema visitors: visit_ident (base case via TY_DEPENDENT), visit_binary,
visit_unary, visit_assign, visit_ternary, visit_member, visit_subscript,
visit_call (all propagate from children).

### Stage 2: Phase-2 Lookup in clone.c ✓

- Removed SubstMap-tag-match fallbacks (dead code after Stage 0 scope fix)
- Added Phase-2 qualified lookup for ND_QUALIFIED: after token
  substitution, look up the member in the concrete class_region and
  set resolved_type (N4659 §6.4.3 [basic.lookup.qual])

### Stage 3: Sema Member Override ✓

The `already_typed` override for class members is NOT a shortcut —
it correctly implements N4659 §6.4.5 [class.qual]. Relabeled.

### Stage 4: Codegen Method Dispatch ✓

Replaced blanket `n_template_args > 0` with class_def member scan
(N4659 §6.4.5). Narrowed the template-args fallback to only fire
when class_def is absent (unpatched Type copies from function params).

### Stage 5: Cleanup ✓

- Removed 1-char uppercase heuristics from stmt.c, instantiate.c
- Removed SubstMap-tag-match from clone.c needs_subst + arg substitution
- Removed TODO(seafront#dep-scope) × 4, TODO(seafront#two-phase) × 1,
  TODO(seafront#tmpl-region-patch) × 2
- Updated all comments with proper N4659 references

## Heuristics Remaining

### Standard-conforming (not shortcuts)

- **stmt.c §17.7/5**: dependent qualified name without `typename` → expression
- **sema.c §6.4.5**: class member lookup uses declaration type

### Narrowed shortcuts (with standard justification)

- **emit_c.c vNULL → {0}**: gcc-specific conversion operator lowering
  (C99 §6.7.8/21). Only applies to the `vNULL` identifier.
- **emit_c.c arg-type mangle**: qualified static calls and unresolved
  method calls use call-site arg types instead of decl param types
  (N4659 §16.3 [over.match] says decl types). Only fires when
  ND_QUALIFIED lacks sema resolution.
- **emit_c.c template method fallback**: `n_template_args > 0` → method
  call when class_def is absent. Sound for C++03 templates.

## What's Deferred

- SFINAE / enable_if (not in gcc 4.8 C++03 headers)
- ADL in dependent contexts
- Variadic templates
- Full value-dependent expression tracking
- ND_QUALIFIED sema resolution (would eliminate arg-type mangle shortcut)
