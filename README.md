# xv6 OS Projects — System Calls, Schedulers & Virtual Memory

> **Course:** Operating Systems Mini Projects  
> **Author:** Pariza Goyal (Roll No: 2024101015)  
> **Platform:** xv6-riscv — a clean, teaching OS running on RISC-V via QEMU

---

## Repository Structure

```
xv6-project/
├── xv6/          ← Mini Project 1 deliverables (patch + report + test)
└── A/            ← Mini Project 2 full modified xv6 source
```

---

##  `xv6/` — Mini Project 1: System Calls & Schedulers

This folder contains the **deliverables** for Mini Project 1. It does **not** hold the full OS source — instead it contains the patch file, test program, and report that document the changes made to xv6.

### What's Inside

| File | Description |
|---|---|
| `xv6_modifications.patch` | Git patch with all kernel changes for Part A and Part B |
| `readcount.c` | User-space test program for the `getreadcount` syscall |
| `report.md` | Full write-up covering implementation details and performance results |

### What Was Implemented

#### Part A — `getreadcount` System Call
A new kernel system call that tracks the **total number of bytes read** across all processes system-wide, using a spin-lock protected global counter.

- `total_read_bytes` counter added to `kernel/proc.c`
- `sys_read()` in `kernel/sysfile.c` incremented on every read
- `sys_getreadcount()` added in `kernel/sysproc.c`
- Wired up through `syscall.h`, `syscall.c`, `user/user.h`, `user/usys.pl`

#### Part B — Scheduler Implementations
Two alternative CPU schedulers were added alongside the default Round Robin, selectable at compile time via `SCHEDULER=`.

| Scheduler | Flag | Behaviour |
|---|---|---|
| Round Robin (default) | *(none)* | Preemptive time-sliced, avg wait ≈ 93 ticks |
| FCFS | `SCHEDULER=FCFS` | Non-preemptive, arrival order, avg wait ≈ 48 ticks |
| CFS | `SCHEDULER=CFS` | vruntime-based fairness, avg wait ≈ 97 ticks |

A `wait_stat` system call was also added so parent processes can collect `rtime` (run time) and `wtime` (wait time) of their children.

### How to Apply the Patch

1. Clone the upstream xv6-riscv repository:
   ```bash
   git clone https://github.com/mit-pdos/xv6-riscv.git
   cd xv6-riscv
   ```

2. Apply the patch:
   ```bash
   git apply ../xv6/xv6_modifications.patch
   ```

3. Build and run with the desired scheduler:
   ```bash
   # Default Round Robin
   make qemu

   # FCFS
   make qemu SCHEDULER=FCFS

   # CFS
   make qemu SCHEDULER=CFS
   ```

4. Inside the QEMU shell, test the `getreadcount` syscall:
   ```bash
   $ readcount
   ```

---

##  `A/` — Mini Project 2: Virtual Memory Extensions

This folder contains the **complete modified xv6-riscv source code** for Mini Project 2. All kernel and user-space changes are already applied — you can build and run it directly.

### What's Inside

```
A/
├── kernel/       ← Modified kernel source (VM, page fault, proc, syscalls)
│   ├── pagefault.c       ← Demand paging & lazy allocation
│   ├── pagefault_lru.c   ← LRU page replacement policy
│   ├── vm.c              ← Virtual memory management
│   ├── proc.c / proc.h   ← Extended process struct
│   └── ...               ← Full kernel source
├── user/         ← User-space programs and test utilities
│   ├── test_memstat.c    ← Memory statistics test
│   ├── logstress.c       ← Log stress test
│   └── ...
├── mkfs/         ← Filesystem image builder
├── Makefile      ← Build system (supports scheduler flags)
└── test-xv6.py   ← Automated test runner
```

### Key Features Added

- **Demand Paging / Lazy Allocation** — Pages are only allocated when first accessed (page fault handler in `kernel/pagefault.c`)
- **LRU Page Replacement** — An LRU-based eviction policy implemented in `kernel/pagefault_lru.c`
- **Memory Statistics Syscall** — `memstat` syscall to query free/used physical memory (test: `user/test_memstat.c`)
- **Orphan Process Handling** — `user/dorphan.c` and `user/forphan.c` for testing orphan/zombie process scenarios

### Prerequisites

You need the RISC-V toolchain and QEMU:

```bash
# macOS (Homebrew)
brew tap riscv-software-src/riscv
brew install riscv-tools
brew install qemu
```

Or follow the full guide: https://pdos.csail.mit.edu/6.1810/

### How to Build and Run

```bash
cd A/

# Build and launch in QEMU (default Round Robin scheduler)
make qemu

# Build with FCFS scheduler
make qemu SCHEDULER=FCFS

# Build with CFS scheduler
make qemu SCHEDULER=CFS

# Run automated tests
python3 test-xv6.py

# Clean build artifacts
make clean
```

### Running in QEMU

Once QEMU starts, you'll see the xv6 shell prompt (`$`). You can then run any of the user programs:

```
$ ls               # list files
$ readcount        # test getreadcount syscall (if patch applied)
$ test_memstat     # test memory statistics syscall
$ logstress        # run log stress test
$ usertests        # run full xv6 user test suite
```

To exit QEMU: press `Ctrl-A` then `X`.

---

## Tech Stack

| Component | Details |
|---|---|
| **OS Base** | xv6-riscv (MIT PDOS) |
| **Architecture** | RISC-V 64-bit |
| **Emulator** | QEMU (`qemu-system-riscv64`) |
| **Language** | C (ANSI C), RISC-V Assembly |
| **Build System** | GNU Make |
| **Toolchain** | `riscv64-unknown-elf-gcc` |

---

## References

- [xv6-riscv source](https://github.com/mit-pdos/xv6-riscv)
- [MIT 6.1810 Operating Systems Engineering](https://pdos.csail.mit.edu/6.1810/)
- [xv6 Book (RISC-V edition)](https://pdos.csail.mit.edu/6.1810/2023/xv6/book-riscv-rev3.pdf)
