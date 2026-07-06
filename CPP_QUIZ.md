# C++ Self-Test — 40 Questions

Covers everything from this session: atomics, mutexes, condition variables,
memory ordering, lambdas, smart pointers, threads, pointers, arrays, and the
concepts behind `main.cpp` and `CPP_PRIMITIVES.md`.

Answer each in your own words first, then check the **Answer Key** at the bottom.
Don't peek until you've committed to an answer — the goal is to find the gaps.

---

## Part 1 — `std::atomic` (Q1–6)

**Q1.** Why is a plain `int` shared between two threads unsafe, even for a simple
read on one thread and write on another? Name the three specific problems.

**Q2.** What does `std::atomic` tell the compiler and CPU to do (or not do)?

**Q3.** Why can you NOT use `std::atomic` on a struct with two fields that must
stay consistent with each other?

**Q4.** What is the equivalent of `std::atomic` when you need to protect a struct
(multiple fields together)?

**Q5.** In this code, why is it still a bug even though both variables are atomic?
```cpp
if (write_idx - read_idx < MAX_DEPTH)
    q.push(...);
```

**Q6.** True or false: on x86, a load of a naturally-aligned `size_t` is already
atomic at the hardware level. What does `std::atomic` add on top?

---

## Part 2 — `std::mutex` (Q7–12)

**Q7.** "A mutex locks data." Is this statement correct? Explain what a mutex
actually locks.

**Q8.** What guarantees that `shared_data` is actually protected by `mtx`? Is it
the compiler?

**Q9.** Walk through what happens at the OS level when Thread A holds a mutex and
Thread B tries to acquire it.

**Q10.** Why do BOTH the reader and the writer need to lock the mutex? Isn't
locking just the writer enough?

**Q11.** When should you use a mutex instead of an atomic?

**Q12.** What is the rule about how long you should hold a mutex, and why?

---

## Part 3 — Atomic vs Mutex (Q13–15)

**Q13.** Fill in the table from memory:

| | atomic | mutex |
|---|---|---|
| What it protects | ? | ? |
| Cost | ? | ? |
| Blocks the thread? | ? | ? |

**Q14.** In the SPSC ring buffer, why is a mutex unnecessary? What makes atomics
alone sufficient?

**Q15.** What is the ONE hard constraint that must hold for the lock-free ring
buffer to be safe? What breaks if you violate it?

---

## Part 4 — Memory order (Q16–20)

**Q16.** What does `memory_order_release` on a store guarantee?

**Q17.** What does `memory_order_acquire` on a load guarantee?

**Q18.** Does acquire/release make a thread pause or wait? If not, what does it
actually do?

**Q19.** In the ring buffer, if the producer did NOT use `release` when storing
`write_idx`, what specific bug could the consumer hit?

**Q20.** Match each to its use: `relaxed`, `release`, `acquire`, `seq_cst`.
- Which is the default and safest but slowest?
- Which would you use for a statistics counter that guards no other data?

---

## Part 5 — Lock wrappers & scope (Q21–24)

**Q21.** What is the one capability `unique_lock` has that `lock_guard` does not,
and why does `condition_variable` require it?

**Q22.** In this code, at exactly which line is the mutex released? Why is `batch`
declared outside the braces?
```cpp
std::vector<Sample> batch;
{
    std::unique_lock<std::mutex> lock(mtx);
    batch = std::move(queue.front());
    queue.pop();
}
write_to_disk(batch);
```

**Q23.** What does RAII stand for, and how do `lock_guard` and `unique_ptr` both
demonstrate it?

**Q24.** If a `break` fires inside a locked scope, is the mutex released? Explain
why or why not.

---

## Part 6 — Condition variables (Q25–31)

**Q25.** In one sentence each: what does the mutex do vs what does the condition
variable do in a producer-consumer setup?

**Q26.** Why do you need a condition variable if you already have a mutex? What
goes wrong if you remove the CV and just use a mutex + while loop?

**Q27.** Explain step by step what `cv.wait(lock, predicate)` does internally.

**Q28.** What is a "spurious wake-up" and how does the predicate argument protect
against it?

**Q29.** Why must you hold the mutex when calling `cv.wait`? Describe the race
condition that happens without it.

**Q30.** What is the difference between `notify_one` and `notify_all`? When would
you use each?

**Q31.** In this predicate, explain where the return value goes — who consumes it?
```cpp
cv.wait(lock, [&] { return !q.empty() || stop; });
```

---

## Part 7 — Lambdas (Q32–35)

**Q32.** What does the capture list do? Explain the difference between `[&]`,
`[=]`, and `[]`.

**Q33.** Why must the `cv.wait` predicate use `[&]` and not `[=]`? What would go
wrong with `[=]`?

**Q34.** What will this print, and why?
```cpp
int a = 5;
auto f = [a]() { return a + 3; };
a = 100;
std::cout << f();
```

**Q35.** Why is capturing a local variable by reference (`[&x]`) safe inside
`cv.wait` but dangerous when passed to a detached `std::thread`?

---

## Part 8 — Smart pointers & memory (Q36–40)

**Q36.** What problem do smart pointers solve compared to raw `new`/`delete`?

**Q37.** What is the key difference between `unique_ptr` and `shared_ptr`? Which
should you use by default and why?

**Q38.** What does a `weak_ptr` do that a `shared_ptr` doesn't, and name one
situation where you need it.

**Q39.** Why is this a dangling pointer? Do smart pointers fix this particular
problem?
```cpp
int* getPointer() {
    int localVar = 42;
    return &localVar;
}
```

**Q40.** What will this print, and why is the answer NOT an error?
```cpp
shared_ptr<int> p1(new int(30));
shared_ptr<int> p2;
p2 = p1;
cout << *p1 + *p2;
```

---
---

# Answer Key

<details>
<summary>Click to reveal (or scroll) — answer first, then check</summary>

**A1.** (1) Compiler may cache it in a register and never re-read from RAM.
(2) CPU may reorder reads/writes. (3) A 64-bit write may split into two 32-bit
instructions, so another thread reads a half-written value.

**A2.** Never cache in a register (always go to memory); never reorder past
another atomic operation; make the read/write indivisible (one instruction on
x86 for register-sized types).

**A3.** The CPU can only atomically read/write one register-sized value (~8 bytes
on x64). A struct with multiple fields needs multiple instructions — another
thread can run in between and see half-old/half-new data. Also you usually need
the fields consistent *together*, which a single atomic can't guarantee.

**A4.** A `std::mutex` (with `lock_guard`/`unique_lock`).

**A5.** The check (`write_idx - read_idx < MAX_DEPTH`) and the push are two
separate steps. Another thread can change `read_idx` between the check and the
push. Two atomics don't make a multi-step operation indivisible — you'd need a
mutex for that.

**A6.** True. `std::atomic` adds the compiler barriers that prevent
register-caching and instruction reordering — the hardware atomicity alone isn't
enough because the compiler could still optimize incorrectly.

**A7.** Incorrect. A mutex locks a *region of code*, not data. There is no
built-in link between a mutex and any variable — protection only exists because
every thread agrees to acquire the mutex before touching the shared data.

**A8.** Not the compiler — it's a *convention* that every thread follows. The
compiler does not enforce that you locked before touching `shared_data`.

**A9.** Thread A takes the mutex → it's HELD. Thread B tries to take it → the OS
puts B to sleep. When A leaves the scope → mutex released → OS wakes B → B takes
it and proceeds.

**A10.** Locking only the writer is still a data race: the reader could read
while the writer is mid-write. Both sides must lock so they take turns and never
touch the data simultaneously.

**A11.** When you need to protect multiple variables together, or a multi-step
sequence that must be indivisible (e.g. check-then-push, front-then-pop).

**A12.** Hold it for the minimum time needed — just move data out, then release.
Never do heavy computation, disk I/O, or network calls while holding it, because
that blocks the other thread for the entire duration.

**A13.**
| | atomic | mutex |
|---|---|---|
| Protects | a single variable (one indivisible read/write) | a block of code (one thread at a time) |
| Cost | single CPU instruction | OS call, possible thread sleep |
| Blocks? | never | yes, if another thread holds it |

**A14.** Each thread owns exactly one index (producer owns `write_idx`, consumer
owns `read_idx`). Neither writes the other's index. The only cross-thread sharing
is *reading* the other's index — a single variable read, which is exactly what
atomic handles.

**A15.** Exactly one producer and one consumer (SPSC). Add a second producer and
you get a data race on `write_idx` (and on the slot itself) — then you'd need a
mutex or a different structure.

**A16.** Everything written *before* the release store is guaranteed visible to
any thread that does a matching `acquire` load of the same variable.

**A17.** Everything written before the matching `release` store is now visible to
this thread after the acquire load.

**A18.** No — it doesn't pause or wait. It's a reordering fence: it tells the CPU
"do not reorder writes across this point," guaranteeing visibility order between
threads.

**A19.** The CPU could reorder the `write_idx` update to happen *before* the slot
data write. The consumer's acquire load would see the new index, go read the
slot, and find old/garbage data that isn't written yet.

**A20.** `seq_cst` is the default, safest, slowest. `relaxed` for a stats counter
that guards no other data.

**A21.** `unique_lock` can be unlocked and re-locked manually (and temporarily
released). `condition_variable::wait` needs to *atomically release the mutex and
sleep, then re-acquire on wake* — `lock_guard` can't release early, so it can't
do this.

**A22.** The mutex is released at the closing `}` of the inner brace block (when
`lock`'s destructor runs). `batch` is declared outside so it survives after the
lock is released — allowing the slow `write_to_disk(batch)` to run without
holding the mutex.

**A23.** Resource Acquisition Is Initialization. The resource's lifetime is tied
to a stack object; the destructor cleans up automatically on scope exit.
`lock_guard` releases the mutex on destruction; `unique_ptr` deletes the heap
memory on destruction. Both clean up regardless of how the scope exits.

**A24.** Yes, released. `break` doesn't teleport out — C++ runs destructors for
all in-scope locals first (in reverse order). The `unique_lock` destructor runs,
releasing the mutex, *then* execution jumps out of the loop.

**A25.** Mutex: ensures only one thread touches the shared data at a time. CV:
lets a thread sleep until there's actually something to do (a condition becomes
true).

**A26.** The mutex can't put a thread to sleep waiting for a *condition* — without
a CV the consumer must busy-loop (100% CPU) checking the queue, and worse, it
holds/re-grabs the mutex so fast the producer may be starved and can never push
→ effectively a deadlock/livelock.

**A27.** (1) Check predicate. If true, return immediately, don't sleep. (2) If
false, atomically release the mutex and sleep. (3) On `notify`, re-acquire the
mutex and re-check the predicate. (4) If true, return (mutex held). (5) If false,
release and sleep again.

**A28.** The OS can wake a sleeping thread for no reason (spurious wake-up). The
predicate is re-checked on every wake, so if the condition still isn't true the
thread goes back to sleep instead of proceeding on false data.

**A29.** Without the mutex: consumer checks queue (empty) → producer pushes +
notifies → consumer then calls wait() and sleeps → the notification was already
sent, so it's missed → consumer sleeps forever. Holding the mutex makes
"check then sleep" atomic, closing that gap.

**A30.** `notify_one` wakes one waiting thread; `notify_all` wakes all of them.
Use `notify_one` when a single consumer (or any one) can handle it; use
`notify_all` when every waiter needs to re-check (e.g. a shutdown flag with
multiple consumers).

**A31.** The lambda is a predicate returning `bool`. `cv.wait` calls it
internally and consumes the return value to decide whether to sleep or wake. You
never see the bool yourself — your code just resumes after `cv.wait` once the
predicate returns true.

**A32.** The capture list controls which outer-scope variables the lambda can
see. `[&]` = capture all by reference (sees live values, can modify). `[=]` =
capture all by value (private copy frozen at creation). `[]` = capture nothing.

**A33.** The predicate must read the *live* state of the queue and stop flag every
time it's re-checked. `[=]` would snapshot a copy of the queue at creation time —
it would never see items the producer pushes later, so the predicate would always
return false and the thread would sleep forever.

**A34.** Prints **8**. `[a]` captures `a` by value at the moment the lambda is
created (when `a`==5). Changing `a` to 100 afterward doesn't affect the captured
copy. `5 + 3 = 8`.

**A35.** In `cv.wait` the lambda runs *synchronously* right now, while the
captured variables are still alive. A detached thread runs *asynchronously* and
may outlive the local variable — the captured reference then dangles (undefined
behavior).

**A36.** With raw `new` you must manually `delete`, and if you forget or an
exception fires first, the memory leaks. Smart pointers call `delete`
automatically when they go out of scope — heap allocation without manual cleanup.

**A37.** `unique_ptr` = sole ownership, can't be copied (only moved), zero
overhead. `shared_ptr` = multiple owners via an atomic reference count, deleted
when count hits zero, has overhead. Use `unique_ptr` by default — it's cheaper
and expresses clear single ownership.

**A38.** A `weak_ptr` observes an object without owning it (doesn't increment the
ref count / keep it alive). You `lock()` it to get a temporary `shared_ptr` (null
if already deleted). Needed to break circular references between two
`shared_ptr`s, or for non-owning caches.

**A39.** `localVar` is a stack variable destroyed when the function returns, so
the returned address points to freed memory. Smart pointers do NOT fix this — the
problem is returning the address of a stack variable. Fix: return by value (or
allocate on the heap).

**A40.** Prints **60**. `p2 = p1` copies the *pointer*, not the value — both
`shared_ptr`s point to the same single `int` (30), ref count = 2. `*p1 + *p2` =
`30 + 30 = 60`. Not an error because `shared_ptr` is designed to be copyable
(unlike `unique_ptr`).

</details>
