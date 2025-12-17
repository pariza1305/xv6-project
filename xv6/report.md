Mini Project 1: xv6 System Call and Scheduler
Name: Pariza
Roll No: 2024101015
Part A: getreadcount System Call
I successfully implemented the getreadcount system call.
Implementation Steps
Global Counter
Added a global variable total_read_bytes and a struct spinlock read_count_lock in kernel/proc.c.
Initialized the lock inside procinit().
Syscall Hook in read()
Modified sys_read() in kernel/sysfile.c to:
Acquire the lock.
Increment total_read_bytes by the number of bytes read.
Release the lock.
System Call Function
Created sys_getreadcount() in kernel/sysproc.c to safely return the counter value.
User-Space Integration
Added entries in syscall.h, syscall.c, user/user.h, and user/usys.pl.
Testing
The user program readcount.c was used for verification.
On startup, it displayed a non-zero count (due to the shell booting).
Reading a file increased the count correctly, confirming functionality.

Part B: Scheduler Implementation
1. Makefile and Process Statistics
Makefile:
Updated to accept SCHEDULER as a parameter (-DSCHEDULER_FCFS or -DSCHEDULER_CFS).
Statistics Collection:
Added rtime (run time) and wtime (wait time) to struct proc in kernel/proc.h.
Modified usertrap() and kerneltrap() to update:
rtime for every RUNNING process.
wtime for every RUNNABLE process.
Added a new syscall wait_stat so that parents can collect runtime and wait time of children.
2. Scheduler Policies
All scheduling logic was wrapped in #ifdef macros inside kernel/proc.c and kernel/trap.c.
(a) FCFS (First Come First Serve)
Added ctime (creation time) in struct proc, initialized in allocproc().
scheduler() selects the oldest RUNNABLE process (smallest ctime).
Made the scheduler non-preemptive by disabling yield() in traps.
Verified behavior: each process runs to completion before the next begins.
(b) CFS (Completely Fair Scheduler)
Added vruntime, weight, slice_ticks, and ran_in_slice to struct proc.
Implemented nice_to_weight() to map process niceness to scheduling weight.
scheduler() logic:
Select the process with lowest vruntime.
Compute time slice as 48 / num_runnable (minimum 3 ticks).
Log the scheduling decision.
Run process until it uses its slice.
Update its vruntime based on actual ticks run, normalized by weight.
Traps (usertrap, kerneltrap) trigger a yield() only after the slice expires.
Part C: Performance Comparison (CPUS=1)
Using the schedulertest program:
Round Robin (default xv6)
Results:
Average Wait Time ≈ 93 ticks
Average Run Time ≈ 23 ticks
Observation: Low, fairly equal wait times due to frequent preemption.
FCFS
Results:
Average Wait Time ≈ 48 ticks
Average Run Time ≈ 24 ticks
Observation: Clear convoy effect – the first process starts immediately, but every subsequent process waits for the entire previous one to finish, leading to a staircase wait-time pattern.
CFS
Results:
Average Wait Time ≈ 97 ticks
Average Run Time ≈ 25 ticks
Observation: Maintains fairness by always selecting the process with the least executed CPU time. Processes progress more evenly compared to FCFS.
CFS Scheduler Log Verification
Below is a sample snippet showing how CFS selects the process with the lowest vruntime at each tick:
[Scheduler Tick]
PID: 4 | vRuntime: 10
PID: 5 | vRuntime: 10
PID: 6 | vRuntime: 10
PID: 7 | vRuntime: 0
PID: 8 | vRuntime: 0
--> Scheduling PID 7 (lowest vRuntime) for 9 ticks
[Scheduler Tick]
PID: 4 | vRuntime: 10
PID: 5 | vRuntime: 10
PID: 6 | vRuntime: 10
PID: 7 | vRuntime: 9
PID: 8 | vRuntime: 0
--> Scheduling PID 8 (lowest vRuntime) for 9 ticks
This confirms the scheduler consistently chooses the least-used process, achieving fairness.
Final Analysis
Round Robin: Ensures fairness, but context switches are frequent.
FCFS: Simple, but suffers from the convoy effect and unfairness to later processes.
CFS: Provides balanced CPU allocation, prevents starvation, and models Linux’s fairness approach.
Conclusion:
My implementation of getreadcount, FCFS, and CFS in xv6 was successful. Testing verified that:
getreadcount tracks system-wide bytes read accurately.
FCFS runs processes in strict arrival order without preemption.
CFS fairly distributes CPU among processes, confirmed by both statistics and logs.