# C++ Concurrency Primitives — Reference

---

## 1. `std::atomic<T>` — one variable, lock-free

Protects a **single primitive variable** shared between threads without a mutex.

```cpp
std::atomic<bool>   stop(false);      // signal across threads
std::atomic<size_t> write_idx(0);     // ring buffer index
std::atomic<int>    drop_count(0);    // statistic counter
```

### What it actually does

A regular variable is not safe to share between threads because:
- The **compiler** may cache it in a register and never re-read from RAM (optimization)
- The **CPU** may reorder reads and writes for performance
- A 64-bit write may split into two 32-bit instructions → another thread reads a half-written value

`std::atomic` tells both the compiler and CPU:
- Never cache this in a register — always go to memory
- Never reorder this past another atomic operation
- Make the read or write indivisible (one CPU instruction on x86 for register-sized types)

### When to use

Use atomic when you need to share **one simple value** (bool, int, size_t, pointer)
and the operation on it is a single read or single write — not a multi-step sequence.

```cpp
// good — single write on one thread, single read on another
stop.store(true, std::memory_order_relaxed);
while (!stop.load(std::memory_order_relaxed)) { ... }

// bad — two separate atomics don't protect the pair together
if (write_idx - read_idx < MAX_DEPTH)   // read write_idx
    q.push(...);                         // then check read_idx
// another thread can change read_idx between these two lines
// → use a mutex to protect the whole multi-step check+push
```

### Only for primitive types

The CPU can atomically read/write one register-sized value (8 bytes on x64).
Larger types require multiple instructions — another thread can run between them and
see half-old/half-new data. For structs, vectors, or queues: use a mutex.

---

## 2. `std::mutex` — a block of code, not a variable

A mutex does not lock data. It locks a **region of code**.

```cpp
std::mutex mtx;
std::vector<int> shared_data;   // no built-in connection between these two
```

Protection only comes from **every thread agreeing** to always acquire `mtx` before
touching `shared_data`. The compiler does not enforce this agreement.

```cpp
// Thread A — writer
{
    std::lock_guard<std::mutex> lock(mtx);  // blocks until mtx is free, then takes it
    shared_data.push_back(42);
}   // destructor runs → mtx released

// Thread B — reader
{
    std::lock_guard<std::mutex> lock(mtx);  // waits if A is inside
    for (auto x : shared_data) { ... }
}
```

### What happens at the OS level

```
mtx is free
Thread A arrives → takes mtx → mtx is now HELD
Thread B arrives → tries to take mtx → OS puts B to sleep
Thread A finishes scope → releases mtx → OS wakes B
Thread B takes mtx → does its work → releases
```

**Both reads and writes need the lock.** Locking only the writer and not the reader
is still a data race.

### When to use

Use a mutex when you need to protect **multiple variables together**, or when the
operation is a multi-step sequence that must be indivisible:

```cpp
// check size + push must be one atomic operation → mutex required
{
    std::lock_guard<std::mutex> lock(mtx);
    if (q.size() < MAX_DEPTH)
        q.push(std::move(batch));
}
```

---

## 3. Atomic vs Mutex — side by side

| | `std::atomic<T>` | `std::mutex` |
|---|---|---|
| What it protects | A single variable — one indivisible read or write | A block of code — only one thread runs it at a time |
| Cost | Single CPU instruction | OS call, possible thread sleep |
| Blocks the thread | Never | Yes, if another thread holds the lock |
| Use when | Sharing one primitive value (bool, int, size_t) | Protecting multiple variables or a multi-step operation |

### The queue example

In a mutex-guarded queue, the mutex protects three operations as one unit:
check size → pop front → update state. You cannot do this with atomics because
the three steps are not a single indivisible operation.

```cpp
// needs a mutex — three steps must be indivisible
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!q.empty()) {
        batch = std::move(q.front());
        q.pop();
    }
}
```

### The ring buffer example

In a lock-free SPSC ring buffer, each thread owns exactly one index.
The only cross-thread sharing is reading the other thread's index — a single variable
read. That is exactly what atomic is for: no mutex needed.

```cpp
// producer — owns write_idx, reads read_idx
size_t next = (write_idx + 1) & (N - 1);
if (next != read_idx.load(std::memory_order_acquire)) {   // single read → atomic
    slots[write_idx] = data;
    write_idx.store(next, std::memory_order_release);      // single write → atomic
}

// consumer — owns read_idx, reads write_idx
if (read_idx != write_idx.load(std::memory_order_acquire)) {   // single read → atomic
    data = slots[read_idx];
    read_idx.store((read_idx + 1) & (N - 1), std::memory_order_release);
}
```

---

## 4. Memory order — what `acquire` and `release` mean

When you do an atomic store with `memory_order_release`, you guarantee:
**everything written before this store is visible to any thread that does a
matching `acquire` load of the same variable.**

```
Producer:                            Consumer:
  slots[w] = data        ← write      idx = write_idx.load(acquire)  ← sees new idx
  write_idx.store(release) ← publish  data = slots[read_idx]         ← sees slot data
```

Without this, the CPU could reorder the slot write to happen *after* the index update,
and the consumer would read the index (thinking a slot is ready) before the data is
actually there.

| Order | Meaning |
|---|---|
| `relaxed` | No ordering guarantee — just atomicity. Use for counters that don't guard other data. |
| `release` | On a store: everything written before this is visible after a matching acquire. |
| `acquire` | On a load: everything written before the matching release is now visible to this thread. |
| `seq_cst` | Default. Total ordering across all threads. Safest, slowest. |

---

## 5. `lock_guard` vs `unique_lock`

Both are RAII wrappers: lock on construction, unlock on destruction.

| | `lock_guard` | `unique_lock` |
|---|---|---|
| Locks on construction | yes | yes |
| Unlocks on destruction | yes | yes |
| Works with `condition_variable` | no | yes |
| Can unlock early manually | no | yes (`lock.unlock()`) |

Use `lock_guard` for simple critical sections.
Use `unique_lock` whenever you also need a `condition_variable`.

---

## 6. Scope as a lock lifetime control

Curly braces control when the lock destructor runs, which is when the mutex is released:

```cpp
std::vector<Sample> batch;   // declared OUTSIDE — survives after lock is released

{
    std::unique_lock<std::mutex> lock(mtx);   // mutex LOCKED
    batch = std::move(queue.front());          // grab data — fast
    queue.pop();
}   // lock destructor → mutex UNLOCKED

// mutex is free here — slow work does not block other threads
write_to_disk(batch);
```

**Rule: hold the mutex for the minimum time needed — just move data out, then release.**
Never do heavy computation, disk I/O, or network calls while holding a lock.

---

## 7. `std::condition_variable` — sleep until something is true

A condition variable lets a thread **sleep until another thread signals it**.
It is always used together with a `mutex` — never alone.

### The problem it solves

Without a condition variable, a consumer thread that is waiting for data must
busy-loop, burning CPU doing nothing:

```cpp
while (q.empty()) {}   // spins at 100% CPU until producer pushes something
```

A condition variable replaces this with an OS-level sleep — zero CPU while waiting.

---

### The three operations

```cpp
std::condition_variable cv;

cv.wait(lock, predicate);   // consumer: sleep until predicate is true
cv.notify_one();            // producer: wake one sleeping thread
cv.notify_all();            // producer: wake ALL sleeping threads
```

---

### `cv.wait` — what it actually does internally

```cpp
cv.wait(lock, [&] { return !q.empty() || stop; });
```

This is NOT just "sleep until notified." Internally it does:

```
1. check predicate() — is the condition already true?
   YES → return immediately, do not sleep
   NO  → atomically: release the mutex AND put thread to sleep

   (while sleeping: mutex is free, producer can push data)

   when cv.notify_one() is called:
     → re-acquire the mutex
     → check predicate() again
     YES → return (thread is awake, mutex is held)
     NO  → release mutex, go back to sleep  ← spurious wake protection
```

The predicate re-check on every wake-up is critical — the OS can wake a thread
for no reason on some platforms (**spurious wake-up**). Without the predicate,
the consumer would proceed even though the queue is still empty.

---

### Why `wait` needs the mutex

There is a race condition if you don't hold the mutex during the sleep decision:

```
without mutex:
  consumer checks q.empty() → true (empty)
  producer pushes item
  producer calls notify_one()       ← notification sent, nobody sleeping yet
  consumer calls wait() → sleeps    ← missed the notification, sleeps forever
```

Holding the mutex makes "check then sleep" one atomic action — the producer
cannot push and notify between your check and your sleep.

---

### Full producer-consumer pattern

```cpp
std::queue<Batch> q;
std::mutex mtx;
std::condition_variable cv;
bool stop = false;

// Producer thread
{
    std::lock_guard<std::mutex> lock(mtx);
    q.push(std::move(batch));
}                       // mutex released BEFORE notify — avoids immediate re-block
cv.notify_one();        // wake the consumer

// Consumer thread
while (true) {
    Batch batch;
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return !q.empty() || stop; });

        if (q.empty()) break;   // stop was set and queue is drained

        batch = std::move(q.front());
        q.pop();
    }                   // mutex released here
    cv.notify_one();    // unblock producer if it was waiting on MAX_DEPTH
    process(batch);     // heavy work done outside the lock
}
```

---

### `notify_one` vs `notify_all`

| | `notify_one` | `notify_all` |
|---|---|---|
| Wakes | One waiting thread | All waiting threads |
| Use when | One consumer, or any one consumer can handle the work | All consumers need to re-check (e.g. shutdown signal) |

In this project `notify_one` is used because there is exactly one consumer thread.
For shutdown (`stop_signal_called = true`) you would call `notify_all` if there
were multiple consumers, so every one of them wakes up and exits.

---

### Condition variable vs busy-loop vs sleep

| Method | CPU while waiting | Latency to wake | Use |
|---|---|---|---|
| `while (!ready) {}` | 100% | ~0 (instant) | Never — wastes CPU |
| `while (!ready) { sleep(1ms); }` | ~0 | up to 1ms | Rarely — adds fixed delay |
| `cv.wait` | 0 | OS scheduler delay (~0.1ms) | Standard — correct approach |

---

## 8. Lambda functions

A lambda is an **anonymous function** you define inline, right where you need it,
instead of writing a separate named function somewhere else.

### Basic syntax

```cpp
[capture](parameters) -> return_type { body }
```

```cpp
// named function — defined far away, called here
bool is_ready() { return !q.empty() || stop; }
cv.wait(lock, is_ready);

// lambda — defined right here, same meaning
cv.wait(lock, []() { return !q.empty() || stop; });
```

Both do the same thing. The lambda version is easier to read because the logic
is right next to the call that uses it.

---

### The capture list `[ ]`

The capture list controls which variables from the surrounding scope the lambda
can see. A lambda body is a separate function — it cannot see outer variables
unless you explicitly capture them.

| Syntax | Meaning |
|---|---|
| `[]` | Capture nothing — lambda can only use its own parameters and globals |
| `[&]` | Capture everything by **reference** — reads and writes the original variables |
| `[=]` | Capture everything by **value** — gets a private copy at the moment of creation |
| `[&q, &stop]` | Capture specific variables by reference |
| `[x]` | Capture only `x` by value |

```cpp
int threshold = 10;

auto by_ref   = [&]()  { return threshold > 5; };   // reads the real threshold
auto by_value = [=]()  { return threshold > 5; };   // reads a snapshot taken here
auto specific = [&threshold]() { return threshold > 5; };  // same as [&] but explicit

threshold = 20;
by_ref();    // returns true  — sees the updated 20
by_value();  // returns true  — still sees the snapshot 10... wait, 10 > 5 is true
             // change threshold = 3 to see the difference more clearly
```

**`[&]` is the most common in this codebase** — used in `cv.wait` predicates so
the lambda can read the live queue and stop flag:

```cpp
cv.wait(lock, [&] {
    return !sq.q.empty() || stop_signal_called.load();
    //      ^^^^^^ live reference — sees the current state of sq.q
});
```

If you used `[=]` here, the lambda would capture a copy of `sq.q` at the moment
`cv.wait` was first called — it would never see new items pushed by the producer,
and the predicate would always return false.

---

### Return type

The `-> return_type` part is almost always omitted — the compiler deduces it:

```cpp
[&] { return !q.empty(); }         // compiler deduces bool
[&] { return q.size(); }           // compiler deduces size_t
[&] -> bool { return !q.empty(); } // explicit — rarely needed
```

Only write the return type when the body has multiple return statements that
could return different types, or when you want to force a specific type.

---

### Lambdas as thread functions

You can pass a lambda directly to `std::thread` instead of a named function:

```cpp
// named function approach
void csv_writer(SampleQueue& sq) { ... }
std::thread t(csv_writer, std::ref(sq));

// lambda approach — same result
std::thread t([&sq] {
    // csv_writer logic here
});
```

The lambda captures `sq` by reference (`[&sq]`), so the thread sees the real
`SampleQueue`, not a copy. Equivalent to passing `std::ref(sq)` with a named function.

---

### Why `[&]` is fine inside `cv.wait` but risky with threads

With `cv.wait`, the lambda runs **synchronously** on the same thread, right now,
before `wait` returns. The outer variables are guaranteed to be alive.

With `std::thread`, the lambda runs **asynchronously** on a new thread. If you
capture a local variable by reference and that variable goes out of scope before
the thread finishes, the thread reads a dangling reference — undefined behavior.

```cpp
void launch() {
    int x = 5;
    std::thread t([&x] { process(x); });   // DANGEROUS — x may be destroyed
    t.detach();                             // before the thread runs
}

// safe alternatives:
std::thread t([x] { process(x); });        // capture by value — thread gets its own copy
std::thread t([&sq]{ ... }, std::ref(sq)); // safe only if sq outlives the thread (it does here)
```

In this project `sq` is declared in `main()` and the thread is `join()`ed before
`main()` returns, so capturing `sq` by reference is safe.

---

## 8. Smart pointers — automatic memory management

### The problem with raw pointers

When you allocate on the heap with `new`, you must manually call `delete`.
If you forget, or an exception fires before you reach `delete`, the memory is
leaked — it stays allocated forever until the process exits.

```cpp
int* p = new int(42);
// ... if an exception fires here, delete never runs
delete p;   // must remember this — easy to miss
```

Smart pointers wrap a raw pointer and **automatically call `delete`** when the
pointer goes out of scope. You get heap allocation without manual cleanup.

---

### `std::unique_ptr` — sole ownership

One owner. When the `unique_ptr` is destroyed, it deletes what it points to.
Cannot be copied — only moved (ownership transfers).

```cpp
#include <memory>

std::unique_ptr<int> p = std::make_unique<int>(42);

// no delete needed — destructor calls it automatically when p goes out of scope
std::cout << *p;      // dereference just like a raw pointer
std::cout << p.get(); // get the raw pointer if you need it (rarely needed)
```

**Cannot copy** — enforces that exactly one thing owns the memory:

```cpp
auto p1 = std::make_unique<int>(42);
auto p2 = p1;            // COMPILE ERROR — copying unique_ptr is deleted
auto p2 = std::move(p1); // OK — p1 gives up ownership, p2 takes it, p1 becomes null
```

**Use `unique_ptr` for everything by default.** It has zero overhead compared
to a raw pointer — same size, same speed.

---

### `std::shared_ptr` — shared ownership

Multiple owners. Internally keeps a **reference count** — the number of
`shared_ptr`s pointing at the same object. When the count drops to zero,
the object is deleted.

```cpp
std::shared_ptr<int> a = std::make_shared<int>(42);
std::shared_ptr<int> b = a;   // OK — both own it, ref count = 2

// when b goes out of scope: ref count = 1, object still alive
// when a goes out of scope: ref count = 0, object deleted
```

**Cost**: the reference count is an atomic integer — every copy and destruction
does an atomic increment/decrement. Not free, but usually negligible.

**Use `shared_ptr`** when you genuinely need multiple owners and can't reason
about which one will outlive the others. Don't use it by default — `unique_ptr`
is cheaper and clearer about ownership.

---

### `std::weak_ptr` — observe without owning

A `weak_ptr` points to an object managed by a `shared_ptr` but does **not**
increment the reference count. It does not keep the object alive.

To actually use the object, you must `lock()` the `weak_ptr` — this gives you
a temporary `shared_ptr`. If the object was already deleted, `lock()` returns null.

```cpp
std::shared_ptr<int> owner = std::make_shared<int>(42);
std::weak_ptr<int>   observer = owner;   // does not increment ref count

// later — owner may or may not still exist
if (auto p = observer.lock()) {   // p is a shared_ptr, valid only inside this if
    std::cout << *p;              // safe — object is alive
}                                 // p destroyed here, ref count back down
// if owner was already deleted, lock() returns null → if-body skipped
```

**Use `weak_ptr`** to break circular references (two `shared_ptr`s pointing at
each other would keep both alive forever) or to cache something without
preventing its deletion.

---

### Smart pointers vs dangling pointers

The dangling pointer problem from the previous example:

```cpp
int* getDanglingPointer() {
    int localVar = 42;
    return &localVar;   // returns address of stack variable — destroyed on return
}
```

Smart pointers don't help here — the issue is returning the address of a stack
variable. The fix is to return by value.

Where smart pointers **do** prevent dangling:

```cpp
// raw pointer — easy to dangle
int* raw = new int(42);
delete raw;
*raw = 5;   // dangling — raw still holds the address but memory is freed

// unique_ptr — cannot dangle this way
auto p = std::make_unique<int>(42);
// p goes out of scope → delete called automatically
// p is now destroyed — you can't even access it anymore
```

---

### Which to use

| Situation | Use |
|---|---|
| Single clear owner, no sharing | `unique_ptr` |
| Multiple owners, lifetime unclear | `shared_ptr` |
| Observe a `shared_ptr` without owning it | `weak_ptr` |
| Stack variable, no heap needed | Plain value — no pointer at all |
| Raw `new` / `delete` | Almost never in modern C++ |

---

### RAII — the pattern behind smart pointers

Smart pointers are an example of **RAII** (Resource Acquisition Is Initialization):
tie the lifetime of a resource (heap memory, file handle, mutex lock) to the
lifetime of a stack object. When the stack object is destroyed, the destructor
cleans up the resource automatically.

You have already used RAII in this project:

```cpp
std::lock_guard<std::mutex> lock(mtx);   // acquires mutex on construction
                                          // releases it on destruction (scope exit)
```

Same idea as `unique_ptr` — the resource (lock / heap memory) is released
automatically when the wrapper goes out of scope, regardless of how the scope
exits (normal return, exception, early break).
