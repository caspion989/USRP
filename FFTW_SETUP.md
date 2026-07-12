# Installing FFTW on Linux

FFTW ("Fastest Fourier Transform in the West") is a C library for computing
DFTs. This guide covers installing it both with and without internet access,
for use as a comparison benchmark against this project's hand-rolled
`fft_inplace()`.

There are two flavors of the library:
- `libfftw3` — double precision (`fftw_execute`, `fftw_plan_dft_1d`, ...)
- `libfftw3f` — single precision / float (`fftwf_execute`, ...)

Since `main.cpp` uses `std::complex<float>`, you want the **float** variant
(`libfftw3f`) for an apples-to-apples comparison. Install both if unsure —
they're small.

---

## 1. Online install (device has internet)

### Debian / Ubuntu (apt)

```bash
sudo apt update
sudo apt install libfftw3-dev libfftw3-doc
```

`libfftw3-dev` pulls in both the double-precision and single-precision
(`libfftw3f`) runtime libs and headers — Debian ships them as one dev
package. Verify:

```bash
dpkg -L libfftw3-dev | grep -E 'fftw3\.h|libfftw3f?\.so'
```

You should see `/usr/include/fftw3.h` and both `.so` symlinks under
`/usr/lib/x86_64-linux-gnu/` (path varies by arch).

### Fedora / RHEL / CentOS (dnf)

```bash
sudo dnf install fftw-devel fftw-libs-single fftw-libs-double
```

### Arch Linux (pacman)

```bash
sudo pacman -S fftw
```

Arch's single package includes both precisions.

### Compiling/linking against it

```bash
g++ myprog.cpp -o myprog -lfftw3f -lm      # single precision
g++ myprog.cpp -o myprog -lfftw3 -lm       # double precision
```

Include header: `#include <fftw3.h>`

---

## 2. Building from source (internet available, but want a specific version or no package manager)

```bash
curl -O https://www.fftw.org/fftw-3.3.10.tar.gz
tar xzf fftw-3.3.10.tar.gz
cd fftw-3.3.10

# Build float variant
./configure --enable-float --enable-shared
make -j"$(nproc)"
sudo make install

# Build double variant too (separate configure/build, same source tree)
make distclean
./configure --enable-shared
make -j"$(nproc)"
sudo make install

sudo ldconfig
```

`--enable-shared` gives you `.so` files; drop it for static `.a` only.
Add `--enable-sse2` / `--enable-avx2` / `--enable-avx512` (x86) if you want
FFTW to use those instruction sets explicitly — otherwise `configure`
auto-detects what the build host supports.

---

## 3. Offline install (target device has no internet)

The core idea: fetch everything on a machine that **does** have internet,
transfer the files over via USB drive / SCP / etc., then install locally on
the offline target. Three approaches, in order of preference:

### Option A — Download `.deb`/`.rpm` packages on a connected machine, transfer, install offline

Best when the offline target runs the same distro/version as a connected
machine you have access to.

**On a connected machine (same distro/arch as the target):**

```bash
# Debian/Ubuntu
mkdir fftw_offline && cd fftw_offline
apt-get download libfftw3-dev libfftw3-double3 libfftw3-single3 libfftw3-bin
# apt-get download does NOT install anything — just downloads .deb files to cwd

# Fedora/RHEL
mkdir fftw_offline && cd fftw_offline
dnf download --resolve fftw-devel fftw-libs-single fftw-libs-double
```

Copy the `fftw_offline/` folder to a USB drive.

**On the offline target:**

```bash
# Debian/Ubuntu
sudo dpkg -i *.deb
# If dpkg complains about missing dependencies:
sudo apt install -f    # only works if apt has a local/offline repo configured;
                        # otherwise download the missing deps too and re-copy

# Fedora/RHEL
sudo rpm -ivh *.rpm
# or, to let it resolve deps from the local folder:
sudo dnf install ./*.rpm
```

Check `ldd $(which your_binary)` afterward to confirm `libfftw3.so`/
`libfftw3f.so` resolve.

### Option B — Download the source tarball, transfer, build offline

Best when the target's distro/version doesn't match anything you have
connected access to, or you don't have root/sudo package access on the
target.

**On a connected machine:**

```bash
curl -O https://www.fftw.org/fftw-3.3.10.tar.gz
```

Transfer `fftw-3.3.10.tar.gz` to the offline device (USB, `scp` over a LAN
if the offline device just lacks *internet* but has local network, etc.).

**On the offline target**, you need a working C compiler and `make`
already present (these should be part of the base dev toolchain — verify
with `gcc --version` and `make --version` before transferring anything,
since if they're missing too you'll need to bundle `build-essential`
packages the same way as Option A):

```bash
tar xzf fftw-3.3.10.tar.gz
cd fftw-3.3.10
./configure --enable-float --enable-shared --prefix=/usr/local
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

`./configure` does not need internet — it only probes the local system
(compiler, headers, CPU features). No network calls happen during
`configure`/`make`/`make install`.

### Option C — Static-link a prebuilt copy into your own binary (no install step at all on target)

Useful if you can't write to `/usr/local` or run `sudo` on the target at all
(locked-down embedded device, etc.).

**On a connected machine**, build FFTW as static libs (matching the
target's CPU arch — cross-compile with `--host=` if target arch differs
from build arch):

```bash
./configure --enable-float --disable-shared --enable-static --prefix="$PWD/out"
make -j"$(nproc)" && make install
```

This produces `out/lib/libfftw3f.a` and `out/include/fftw3.h`. Copy both
files (not the whole source tree) to the target alongside your project, then
compile your own program directly against them — no system install, no
`sudo`, no `ldconfig`:

```bash
g++ myprog.cpp -Iout/include -o myprog out/lib/libfftw3f.a -lm
```

The resulting binary has FFTW baked in and needs nothing further from the
target system at runtime.

---

## Verifying the install (any method)

```bash
cat <<'EOF' > /tmp/fftw_check.c
#include <fftw3.h>
#include <stdio.h>
int main(void) {
    fftwf_complex *in  = fftwf_malloc(sizeof(fftwf_complex) * 8);
    fftwf_complex *out = fftwf_malloc(sizeof(fftwf_complex) * 8);
    fftwf_plan p = fftwf_plan_dft_1d(8, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    fftwf_execute(p);
    fftwf_destroy_plan(p);
    fftwf_free(in); fftwf_free(out);
    printf("FFTW float OK\n");
    return 0;
}
EOF
gcc /tmp/fftw_check.c -o /tmp/fftw_check -lfftw3f -lm && /tmp/fftw_check
```

If it prints `FFTW float OK`, linking works end-to-end.

## Notes specific to this project

- This is a **Windows** project (see `CLAUDE.md` — MSVC/UHD/vcvars64
  toolchain). This guide is for a separate Linux benchmarking exercise, not
  for building `main.cpp` itself, which stays Windows-only.
- For a fair benchmark against `fft_inplace()`, use `FFTW_MEASURE` (not
  `FFTW_ESTIMATE`) when planning, and warm up the plan once outside the
  timed loop — FFTW's plan step itself is slow and shouldn't count against
  it.
