Place an x86_64 Linux kernel image here for `--linux-system`.

The runner auto-detects these filenames:

- `bzImage`
- `vmlinuz`
- `Image`

You can also point the script at an explicit kernel image with:

```bash
./run_all_tests.sh --linux-system --linux-system-kernel /path/to/bzImage
```

Notes:

- The script builds a minimal ext4 rootfs automatically.
- `busybox-static` is used inside the guest as `/bin/sh`.
- The kernel should have `ext4`, `virtio-blk`, and `ttyS0` console support available at boot.
- The guest currently supports `x86_64` only.
