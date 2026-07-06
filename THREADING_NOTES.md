# Real-Time Threading in C++ — Session Notes

---

## 1. The five primitives

```cpp
#include <thread>               // std::thread
#include <mutex>                // std::mutex, std::lock_guard, std::unique_lock
#include <condition_variable>   // std::condition_variable
#include <atomic>               // std::atomic<T>
#include <queue>                // std::queue  (not thread-safe on its own)
```

| Primitive | What it does |
|---|---|
| `std::thread` | Launches a function on a new OS thread |
| `std::atomic<T>` | Lock-free single-variable share between threads |
| `std::mutex` | A flag (held / free) that threads agree to check before touching shared data |
| `std::lock_guard` / `std::unique_lock` | RAII wrappers that lock on construction, unlock on destruction |
| `std::condition_variable` | Lets a thread sleep until another thread signals it |

---

## 2. `std::atomic` — for single primitive variables

Use when you need to share one simple value (bool, int, size_t) between threads
without a mutex:

```cpp
std::atomic<bool> stop(false);

// signal handler (any thread)
stop = true;

// worker loop (another thread)
while (!stop) { ... }
```

The write on one thread is immediately visible to the other.
**Only for primitive types** — for anything larger (struct, vector, queue) use a mutex.

Why not atomic for structs?
- The CPU can only atomically read/write one register-sized value (8 bytes on x64).
- Larger types require multiple CPU instructions. Another thread can run between them,
  seeing half-old / half-new data.
- Even if the CPU could do it, you usually need multiple fields to be consistent
  together — a mutex lets you group any number of operations into one indivisible block.

---

## 3. `std::mutex` — what it actually locks

**A mutex does not lock data. It locks a region of code.**

```cpp
std::mutex mtx;
std::vector<int> shared_data;   // no connection between these two — you enforce the link
```

The only protection comes from every thread agreeing to always acquire `mtx`
before touching `shared_data`. The compiler does not enforce this.

```cpp
// Thread A — writer
{
    std::lock_guard<std::mutex> lock(mtx);  // try to take mtx
                                             // if another thread holds it: sleep here
                                             // until it is released, then continue
    shared_data.push_back(42);
}   // lock destructor runs → mtx released

// Thread B — reader
{
    std::lock_guard<std::mutex> lock(mtx);  // same: waits if A is inside
    for (auto x : shared_data) { ... }
}
```

What happens at the OS level:

```
mtx is free
Thread A arrives → takes mtx → mtx is now HELD
Thread B arrives → tries to take mtx → OS puts B to sleep
Thread A finishes scope → releases mtx → OS wakes B
Thread B takes mtx → does its work → releases
```

**You never manually check "is the mutex free?"**
You just always try to take it. The OS handles the sleeping and waking.

**Both reads and writes need the lock.**
Locking only the writer and not the reader is still a data race.

---

## 4. Scope and RAII — controlling when the lock is released

The curly braces `{ }` control when the `lock_guard` / `unique_lock` is destroyed,
which is when the mutex is released:

```cpp
std::vector<Sample> batch;   // declared OUTSIDE — survives after the lock is released

{
    std::unique_lock<std::mutex> lock(mtx);  // mutex LOCKED
    batch = std::move(queue.front());        // grab data from shared queue
    queue.pop();
}   // lock destructor runs here → mutex UNLOCKED

// mutex is free here — heavy processing does NOT block other threads
process(batch);
```

**Rule: hold the mutex for the minimum time needed — just move data out, then release.**
Never do heavy computation while holding a lock.

---

## 5. `lock_guard` vs `unique_lock`

| | `lock_guard` | `unique_lock` |
|---|---|---|
| Locks on construction | yes | yes |
| Unlocks on destruction | yes | yes |
| Can be used with `condition_variable` | no | yes |
| Can unlock early manually | no | yes |

Use `lock_guard` for simple critical sections.
Use `unique_lock` whenever you also need a `condition_variable`.

---

## 6. `std::condition_variable` — signaling between threads

Used to make a thread sleep until another thread has work for it.

```cpp
std::queue<Batch> q;
std::mutex mtx;
std::condition_variable cv;

// Producer thread
{
    std::unique_lock<std::mutex> lock(mtx);
    q.push(std::move(batch));
}
cv.notify_one();   // wake the consumer

// Consumer thread
{
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return !q.empty() || stop; });
    //  ^--- atomically releases the mutex and sleeps
    //       when notified: re-acquires the mutex, checks the predicate,
    //       returns only if predicate is true
    auto batch = std::move(q.front());
    q.pop();
}
// process batch here, outside the lock
```

The predicate `[&]{ return !q.empty() || stop; }` guards against **spurious wake-ups**:
the OS can wake a thread for no reason on some platforms, so `wait` always
re-checks the predicate and goes back to sleep if it is false.

---

## 7. `std::thread` — lifecycle

```cpp
std::thread t(my_function, arg1, arg2);   // thread starts immediately

// ...

t.join();    // main thread blocks here until t finishes
// must always call either join() or detach() before t goes out of scope
// otherwise the destructor calls std::terminate()
```

**Arguments are copied by default.**
To pass a reference, wrap it with `std::ref()`:

```cpp
MyStruct s;

std::thread t(my_function, std::move(s));  // move — transfers ownership
std::thread t(my_function, std::ref(s));   // reference — thread sees the original
```

A mutex cannot be copied (by design). Always pass the struct that contains it
by `std::ref`.

---

## 8. The producer-consumer pattern (SDR pipeline)

The standard pattern for real-time sample processing:

```
[recv thread — producer]          [dsp thread — consumer]
recv() → recv_buf                   wait on condition_variable
copy valid samples → batch    ──►   pop batch from queue
push batch to SampleQueue           release mutex
notify_one                          apply frequency shift / FFT / etc.
```

Back-pressure: if the DSP thread falls behind, cap the queue depth and make
the producer block rather than growing without bound:

```cpp
// Producer: block if queue is full
cv.wait(lock, [&] { return q.size() < MAX_DEPTH || stop; });
q.push(std::move(batch));
```

---

## 9. RT-specific rules

| Rule | Why |
|---|---|
| Create threads once at startup, keep them alive in a loop | Thread creation is expensive (OS stack allocation, scheduler slot) |
| Keep critical sections tiny — only move data, never process inside a lock | Processing while holding the lock blocks the other thread for the entire duration |
| Release the mutex before `notify_one` | Avoids the notified thread immediately re-blocking on the mutex you still hold |
| Call `uhd::set_thread_priority_safe()` on every worker thread | Prevents the OS from scheduling your DSP thread below a browser tab |
| Track `sample_offset` across batches, not per batch | Restarting the offset resets the phase of the frequency shift → audible clicks or signal artifacts |

---

## 10. `static constexpr` inside a struct

```cpp
struct SampleQueue {
    std::queue<Batch> q;
    std::mutex mtx;
    std::condition_variable cv;
    static constexpr size_t MAX_DEPTH = 32;
};
```

| Keyword | Meaning |
|---|---|
| `static` | Belongs to the type, not to any instance. One copy shared by all. |
| `constexpr` | Computed at compile time — no memory read at runtime. |
| `size_t` | Unsigned integer, same width as a pointer (64-bit on x64). Standard type for sizes and indices. |

Access it as `SampleQueue::MAX_DEPTH`.

---

## 11. Frequency shift — the math and the threading detail

To shift the received spectrum by `f_shift` Hz, multiply each sample by a complex exponential:

```
output[n] = input[n] × e^(j · 2π · f_shift · n / fs)
           = input[n] × (cos(2π·f_shift·n/fs) + j·sin(2π·f_shift·n/fs))
```

`n` is the **absolute sample index** — it must persist across batches.
If you reset `n` to 0 at the start of each batch, the phase jumps at every
batch boundary.

```cpp
size_t sample_offset = 0;   // lives outside the batch loop

while (true) {
    // ... get batch from queue ...

    for (size_t i = 0; i < batch.size(); ++i) {
        float phase = two_pi_f_over_fs * static_cast<float>(sample_offset + i);
        batch[i] *= std::complex<float>(std::cos(phase), std::sin(phase));
    }
    sample_offset += batch.size();   // advance for next batch
}
```

`static_cast<float>` is explicit narrowing from `double` (the type of `M_PI`
and the UHD parameters) to `float`. The inner loop uses `float` because
`std::cos` / `std::sin` on `float` are faster than their `double` versions
at 50 MS/s.

---

## 12. "Real-time" means a hard deadline, not just "fast"

A real-time system has a task that must complete within a bounded time window.
If it misses that window, the consequence is **data loss or failure** — not just slowness.

For a USRP recv loop:
- UHD maintains an internal DMA/USB ring buffer on the hardware side.
- That buffer has a fixed size. If your thread doesn't call `recv()` again before
  it fills, the hardware has nowhere to put new incoming data → samples are dropped (overflow).
- The deadline is not written in your code — it is a property of the hardware buffer
  size and the incoming sample rate.

**Implication**: blocking the recv loop for *any* reason (mutex contention, condition
variable wait, disk I/O) means missing a hard deadline, not just being slow.

---

## 13. Backpressure — right pattern, wrong context

Backpressure means slowing the producer when the consumer can't keep up, to prevent
unbounded memory growth. This is often the correct choice — in a network server,
you'd rather slow down than run out of RAM.

It is the **wrong choice** when the producer has a hard real-time deadline it cannot violate.

```cpp
// This looks safe (caps memory use) but is actually the bug:
cv.wait(lock, [&] { return q.size() < MAX_DEPTH || stop; });
```

When the queue is full, the recv thread blocks here. While it is blocked, nobody is
calling `recv()`, so UHD's hardware buffers fill up and overflow. The backpressure
mechanism — intended to protect memory — is exactly what starves the receive loop.

**Rule for RT recv loops: drop, never block.**
When the consumer can't keep up, sacrifice some data (log it, count the drops) rather
than ever stalling the producer.

```cpp
// RT-safe policy: if queue is full, drop the batch and continue immediately
if (q.size() < MAX_DEPTH)
    q.push(std::move(batch));
else
    ++dropped_batches;   // count it, never block
```

---

## 14. Jitter — why blocking primitives hurt RT threads

**Jitter** is the variance in how long an operation takes. In real-time systems,
jitter matters more than average latency.

`std::mutex` + `std::condition_variable` are **blocking primitives**:
- A thread waiting in `cv.wait()` is suspended by the OS scheduler.
- It only wakes when notified, plus additional scheduling delay to actually get CPU time.
- Even the non-blocked path (briefly acquiring a mutex) can stall unpredictably if the
  OS context-switches you out while you hold the lock, or if another thread contests it.

`uhd::set_thread_priority_safe()` reduces jitter by raising the recv thread's OS
scheduling priority — but that effort is undermined if the same thread then voluntarily
blocks itself on a `cv.wait()`.

---

## 15. Priority inversion

If a **high-priority thread** (recv loop, ideally RT/SCHED_FIFO priority) needs a lock
currently held by a **low-priority thread** (CSV writer), the high-priority thread ends
up waiting on the low-priority one — effectively inverting the priorities you asked the
OS for.

Mutex-protected shared queues between an RT thread and a non-RT thread are a classic
setup for priority inversion.

**Rule**: RT threads should avoid blocking on locks shared with non-RT threads.
A lock-free SPSC ring buffer sidesteps this entirely — no lock, no inversion possible.

---

## 16. Binary I/O vs. formatted text — throughput impact

Formatted text output (CSV) converts every float to ASCII decimal on every sample:

```cpp
file << sample.real() << ',' << sample.imag() << '\n';  // slow: float→text per sample
```

Binary output writes the raw bytes directly:

```cpp
file.write((const char*)buf, num_samples * sizeof(std::complex<float>));  // fast: memcpy speed
```

The difference is typically **5–10x less CPU and I/O work per sample**. A consumer
doing less work per sample naturally keeps up with the producer more often, which means
the drop-path (section 13) is hit less frequently even at high sample rates.

---

## 17. Better RT pipeline architecture

| Component | Mutex + queue (flawed) | Lock-free SPSC ring buffer (RT-safe) |
|---|---|---|
| Producer blocks when full | yes — causes overflow | no — drops or overwrites |
| Lock contention | yes | none |
| OS scheduling involved | yes (cv.wait suspends thread) | no |
| Memory allocation per batch | yes (vector push) | no (pre-allocated slots) |
| Safe for >1 producer | yes | no — strictly 1 producer, 1 consumer |

Minimum viable RT pipeline:
```
recv thread (RT priority):
  recv() → local buffer
  if ring buffer has space: write slot, advance write_idx (atomic release store)
  else: increment drop counter, continue immediately

DSP/write thread:
  spin or yield until write_idx != read_idx  (atomic acquire load)
  process slot
  advance read_idx (atomic release store)
```

Never block the recv thread. Never do I/O or heavy processing inside the recv loop.

