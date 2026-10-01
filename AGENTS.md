# pyxis-fs development

- Read `docs/core.md` for implemented interfaces and `docs/format.md` for the
  accepted format. Coordinate scope with the Pyxis OS filesystem milestone.
- Check branch, worktrees and local changes before editing. Fetch main and use
  focused task branches; publish commits and open Forgejo PRs with `fj`. The owner
  merges. Publish dependency work before updating the Pyxis OS gitlink.
- Use freestanding GNU C23, snake_case, two spaces, K&R control braces and function
  braces on their own line. Prefer explicit fields and failure paths. Do not cast
  disk bytes to C structures or expose host pointers as persistent addresses.
- Keep host libc and kernel/ABI dependencies out of the core. Preserve format
  boundaries and distinguish local validation from allocation/reachability proof.
  Document buffer ownership, contextual preconditions and error behavior beside APIs.
- Agree unresolved format, policy and lifetime choices before implementing them.
  A future milestone is not authorization for placeholders or successful fake work.
- Keep configuration, draft data, benchmark parameters, machine properties and
  implementation choices distinct from contracts. Before adding a validator or
  assertion, identify the deliberate contract that changing the value would violate.
  Keep one authority for each setting or mutable state; resolve overlapping
  ownership instead of preserving it with synchronization and tests. A provisional
  choice becomes a contract only for an explicit, documented architectural reason.
- Validate with ordinary builds and relevant manual use/debugger inspection.
  Do not add tests, self-tests, fixtures, fault injection, CI or output automation
  unless explicitly requested. Report checks actually run and their limits.
- Maintained filesystem tests configure inputs and verify behavior against
  independent expectations. They must not freeze incidental configuration, fixture
  contents or implementation structure. Exact format constants and independently
  expected results for deliberately configured scenarios remain valid. Reassess old
  tests during replacement work; green checks and consistent documentation do not
  justify accidental or duplicated authority.
- Preserve MPL-2.0 coverage and any future upstream provenance/notices. Do not
  duplicate authoritative format definitions in the parent repository.
