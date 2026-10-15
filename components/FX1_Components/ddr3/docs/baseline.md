# DDR3 Controller VP — Specification Baseline

Pinned on 2026-10-14 (P0). Any change of these hashes invalidates the traceability
matrix until it is re-audited.

| Item | Version | SHA-256 |
|---|---|---|
| FTEL_IP_DDR3_CONTROLLER_HAS_v1.0.md | HAS V1.0 review draft | `9ff25e190c69086d78e7e5f207e5f5530e5f28196544ea15219aa9f35ae1e508` |
| FTEL_IP_DDR3_CONTROLLER_MAS_v0.1.md | MAS V0.1 (content ends at §6.2.20) | `64a855501f4b2606c7f9ec3bc6f2cd20cf8ae3324da82b69135cb428a17d911d` |
| DDR3_VP_IMPLEMENTATION_PLAN.md | plan before §3.1 additions | `aa94bef758f714d4b83bca9b87763926d3980962f988fb1cfd962e1d0d287202` |
| DDR3_VP_SUBTASKS.md | 5-subtask split | `bab16095afe125cab8984b0157d879edc5884eeff517801ac405e819e9590808` |

Document location: `/home/duyptt_HW/Documents/work/VP_BK/DDR3/doc` (outside the repository).

Repository: CDC-VP, branch `dev`, base commit `17af185027e015b5d81e2a29a4de8ad1eb562efe`.

Precedence: HAS > MAS > RTL (MAS §1.1). HAS items labelled "Proposed default" keep their
decision IDs (FE.., BE.., RS.., RD-..) in the traceability matrix. They are VP baseline
behaviour, not confirmed silicon behaviour.

Toolchain used for host builds: GCC 11.5.0 (`/usr/bin/g++`), CMake 3.31.8, Ninja 1.10.2,
SystemC 2.3.4 (`/opt/systemc-2.3.4`). Firmware: xPack riscv-none-elf-gcc 15.2.0
(`/opt/toolchains/riscv-none-elf`). The interactive shell PATH contains vendor wrappers
(Synopsys, Xilinx) that shadow `g++`/`python3`; builds must put `/usr/bin` first.
