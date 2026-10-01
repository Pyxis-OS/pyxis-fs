# Initial small RAM baseline

Two serial runs of filesystem revision `4c7dcdd` completed all 40 cases each on
2026-10-01. The [bounded summary](measurements/ram-baseline.json) retains measured
phase counters, preparation/total traffic, trace cross-checks, settings, two timing
samples and resource evidence. No allocator or durability change was made.

The setup was Linux 6.19.10-300.fc44.x86_64 in this KVM VM, 16 vCPUs and 32 GiB
RAM, native GCC 16.2.1 `-O2 -g3`, ext4 tools 1.47.3 and Btrfs tools 6.19.1.
No compiler/test jobs overlapped measured calls. CPU affinity was not pinned;
caches were not dropped. Each case used a fresh 1 GiB logical image, the same
32/256-file population and preparation history. See the [execution contract](ram-validation.md)
for exact calls, synchronization, profile and differing recovery guarantees.

## Submitted write traffic

All values below are KiB submitted from the start marker through final
synchronization, maintenance and native clean unmount. Ranges cover the two
samples. Preparation and formatting are excluded here and reported separately
in the summary. Each row writes 256 KiB (small), 4096 KiB (large), 32 KiB
(overwrite), or 64 KiB (compiler); compiler also performs 48 namespace calls.

| Population | Case | Pyxis operation | ext4 operation | Btrfs operation | ext4 batch | Btrfs batch |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 32 | small | 6,088 | 1,564 | 4,904 | 452 | 1,184 |
| 32 | large | 5,432 | 4,444 | 5,708 | 4,160 | 4,456 |
| 32 | overwrite | 3,084 | 252–260 | 2,720 | 212 | 644 |
| 32 | compiler | 7,744 | 1,772 | 5,864 | 240 | 1,152 |
| 256 | small | 11,096 | 1,568 | 5,000 | 452 | 1,312 |
| 256 | large | 6,716 | 4,448 | 5,804 | 4,160 | 4,488 |
| 256 | overwrite | 5,624 | 252–264 | 2,816 | 212 | 740 |
| 256 | compiler | 15,708 | 1,776–1,848 | 5,896 | 240–272 | 1,280 |

Pyxis and Btrfs byte counts repeated exactly; some ext4 counts varied. Batched
native rows acknowledge weaker intermediate durability and are not equivalent
to Pyxis completed calls. Native traces had no loss, stayed below 16 MiB per
case and matched completed-sector counts after quiescence. The latter is a
cross-check; the primary count is submitted bytes. No simulator duplicate log
writes are included in Pyxis filesystem-request counts.

Pyxis breakdown below is KiB, identical in both samples. Orphan and drain are
maintenance; none wrote data blocks in these cases. Compiler metadata per
namespace call includes its associated writes and cleanup, not an isolated
rename/delete cost.

| Population | Case | Data | User metadata | Orphan metadata | Drain metadata | Total metadata/useful byte |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 32 | small | 256 | 2,564 | 0 | 3,268 | 22.781250 |
| 32 | large | 4,096 | 592 | 0 | 744 | 0.326172 |
| 32 | overwrite | 216 | 1,256 | 0 | 1,612 | 89.625000 |
| 32 | compiler | 64 | 2,432 | 1,024 | 4,224 | 120.000000 |
| 256 | small | 256 | 4,232 | 0 | 6,608 | 42.343750 |
| 256 | large | 4,096 | 1,020 | 0 | 1,600 | 0.639648 |
| 256 | overwrite | 216 | 2,104 | 0 | 3,304 | 169.000000 |
| 256 | compiler | 64 | 4,280 | 1,920 | 9,444 | 244.437500 |

At 256 files, the small-write case submits 43.34375 total bytes per useful byte
versus ext4’s 6.125; its 10,840 KiB of metadata includes 6,608 KiB from drains.
The compiler case submits 15,708 KiB: 327.25 KiB per namespace call when all
associated traffic is divided by its 48 namespace calls (or 245.4375 total bytes
per useful byte). These small histories already miss the ext4 engineering target.

## RAM elapsed observations

The following ranges are milliseconds for measured calls including the final
synchronization/core close. Native unmount is separately timed and counted in
the summary; timing excludes preparation and independent verification. These
numbers include different adapter paths (especially Pyxis simulation/callback
work), are only two observations, and are not NVMe latency or throughput.

| Population | Case | Pyxis operation ms | ext4 operation ms | Btrfs operation ms |
| ---: | --- | ---: | ---: | ---: |
| 32 | small | 378.411–378.967 | 2.956–3.510 | 5.299–7.531 |
| 32 | large | 1,110.808–1,117.129 | 3.723–4.066 | 5.535–6.375 |
| 32 | overwrite | 244.366–245.755 | 0.861–0.951 | 2.856–2.990 |
| 32 | compiler | 497.556–497.669 | 3.722–4.657 | 6.459–10.851 |
| 256 | small | 1,328.460–1,330.392 | 3.362–3.607 | 5.488–6.191 |
| 256 | large | 1,541.565–1,542.175 | 3.741–3.958 | 5.960–6.135 |
| 256 | overwrite | 751.480–751.839 | 0.934–0.951 | 3.563–4.254 |
| 256 | compiler | 1,331.025–1,335.053 | 3.217–4.856 | 6.528–7.126 |

## Storage and validation evidence

Preflight verified the actual tmpfs mount as `noswap`, 2,147,483,648 bytes and
65,536 inodes, actual job membership with `memory.max=4,294,967,296`,
`memory.swap.max=0`, no swapped job memory and hard core limit zero. Baseline
job peaks were 305,025,024 and 318,693,376 bytes. Both had zero max/OOM events
and zero swap at completion. All final lengths, bytes and namespace counts
matched the independent ledger. Source/build artifacts and this bounded summary
are the only persistent task artifacts; payloads, simulator logs and native
images remained in RAM. No system-wide swap or host configuration was changed.

All 111 quick groups passed (315,674,624-byte job peak); all six seed-1 extended
groups passed (492,310,528-byte peak), with no swap or OOM events. Unsafe direct
launches refused for a nonzero core limit, missing TMPDIR and disk-backed TMPDIR.
An initial native run detected trace loss and refused; readiness-driven draining
fixed capture without raising any limits. A deliberate SIGKILL after mounting an
owned RAM-backed ext4 loop and creating a private trace instance left neither
an attached loop nor a trace instance: autoclear, namespace teardown and
ExecStopPost cleanup ran. No claim of forced-OOM testing or physical power-loss
qualification is made. The initial CI configuration at `02a5c1a` refused before
checkout/tests because the rootless runtime rejected `tmpfs,noswap`. The accepted
[quick-only CI mode](ram-validation.md#ci) instead verifies a zero-swap cgroup
and fresh fixture allocation on bounded ordinary tmpfs. A matching local rootless
container passed all 111 groups with a 292,839,424-byte job peak, no swap and no
max/OOM events. Mode checks refused all heavier suites; direct guard probes
confirmed strict-mode refusal without `noswap`, quick-mode allowance, and refusal
of missing, nonempty or disk-backed scratch. At `4d71bca`, Forgejo runner 13.2
ignored the workflow's mount option and the CI guard refused before building
with `dedicated scratch mount is missing`. Trusted runner provisioning remains
pending; the local pass does not establish a CI pass. This changes the CI execution
mechanism; the comparative measurements above used the unchanged strict launcher.

## Proposed next assignment, not implementation approval

Investigate combining the final small orphan data cleanup with its paired
object/orphan-record deletion when the complete private edit fits existing
funded limits. The compiler cases perform 32 orphan publications for 16 one-block
files, plus 64 associated drain publications. Code inspection shows the final
data removal and final object/orphan removal currently take separate batches.
This supplies a specific small target before replacing the allocation-map editor.

A proposal must prove the combined extent/object/orphan changed paths, occupancy
repair, retirement charges, generation funding and workspace fit; retain the
existing bounded path when they do not fit. Preserve retained-handle behavior,
both retained payloads and every uncertain-publication rule. No reservation or
admission increase is proposed. Removing one cleanup and its two drains per file
would remove 48 of the compiler case’s 288 publications; that is a calculated
opportunity, not a measured byte saving. Run matched before/after cases and
maintained recovery tests before accepting any correction. This would leave
small-append/overwrite whole-map costs unresolved. Task 7 and writable deployment
remain blocked, and the next implementation still needs explicit assignment.
