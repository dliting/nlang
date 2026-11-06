"""Extract a deterministic MNIST subset for the examples/mnist_mlp project.

Downloads the official IDX files from a public mirror (or reuses local
copies), takes the FIRST `--train` / `--test` records in file order, and
writes headerless binaries the example program reads directly:

  images -> float32 little-endian, pixel / 255.0, row-major 784 per record
  labels -> one unsigned byte per record

The script is the only place preprocessing happens; the NLang program does
zero conversion. Run with the py313 interpreter:

  python tools/mnist_subset.py                       # 2000/1000 default
  python tools/mnist_subset.py --train 2000 --test 1000
  python tools/mnist_subset.py --source <dir-with-idx>   # offline

Provenance (URLs + upstream SHA256 of the .gz files) is pinned below and
re-verified on every run; the generated README records the exact subset.
"""
import argparse
import gzip
import hashlib
import struct
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "examples" / "mnist_mlp" / "data"

# Official mirror of the long-dead yann.lecun.com files (the one PyTorch
# and TensorFlow builds use). Upstream SHA256 covers the .gz payload.
MIRROR = "https://ossci-datasets.s3.amazonaws.com/mnist/"
# IDX file pair per split: (images name, labels name).
IDX_FILES = {
    "train": ("train-images-idx3-ubyte.gz", "train-labels-idx1-ubyte.gz"),
    "test": ("t10k-images-idx3-ubyte.gz", "t10k-labels-idx1-ubyte.gz"),
}
UPSTREAM = {
    "train-images-idx3-ubyte.gz":
        "440fcabf73cc546fa21475e81ea370265605f56be210a4024d2ca8f203523609",
    "train-labels-idx1-ubyte.gz":
        "3552534a0a558bbed6aed32b30c495cca23d567ec52cac8be1a0730e8010255c",
    "t10k-images-idx3-ubyte.gz":
        "8d422c7b0a1c1c79245a5bcf07fe86e33eeafee792b84584aec276f5a2dbc4e6",
    "t10k-labels-idx1-ubyte.gz":
        "f7ae60f92e00ec6debd23a6088c31dbd2371eca3ffa0defaefb259924204aec6",
}


def sha256_bytes(data: "bytes | bytearray") -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(name: str, cache_dir: Path, source: "Path | None") -> bytes:
    """Return the raw (decompressed) IDX bytes for `name`."""
    local = source / name if source else cache_dir / name
    if not local.is_file():
        if source:
            sys.exit(f"error: {local} not found under --source {source}")
        local.parent.mkdir(parents=True, exist_ok=True)
        url = MIRROR + name
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as r:
            gz = r.read()
        digest = sha256_bytes(gz)
        if digest != UPSTREAM[name]:
            sys.exit(f"error: {name} SHA256 {digest} does not match the "
                     f"pinned upstream value {UPSTREAM[name]}")
        local.write_bytes(gz)
        print(f"  {len(gz)} bytes, sha256 {digest}")
    raw = local.read_bytes()
    return gzip.decompress(raw) if local.suffix == ".gz" else raw


def parse_idx_images(raw: bytes) -> "tuple[int, int, int, memoryview]":
    magic, count, rows, cols = struct.unpack_from(">IIII", raw, 0)
    if magic != 2051:
        sys.exit(f"error: bad image magic {magic} (expected 2051)")
    body = memoryview(raw)[16:]
    if len(body) < count * rows * cols:
        sys.exit(f"error: image file truncated ({len(body)} bytes)")
    return count, rows, cols, body


def parse_idx_labels(raw: bytes) -> "tuple[int, memoryview]":
    magic, count = struct.unpack_from(">II", raw, 0)
    if magic != 2049:
        sys.exit(f"error: bad label magic {magic} (expected 2049)")
    return count, memoryview(raw)[8:]


def write_subset(split: str, n: int, images: bytes, labels: bytes,
                 out_dir: Path) -> "list[str]":
    count, rows, cols, img = parse_idx_images(images)
    lcount, lab = parse_idx_labels(labels)
    if count != lcount:
        sys.exit(f"error: {split} images/labels count mismatch")
    if n > count:
        sys.exit(f"error: asked {n} {split} records, only {count} available")
    pixels = rows * cols

    # Pixels are row-major ubytes; emit row-major float32 little-endian.
    images_out = bytearray(n * pixels * 4)
    for i in range(n):
        base_in = i * pixels
        base_out = i * pixels * 4
        for p in range(pixels):
            struct.pack_into("<f", images_out, base_out + p * 4,
                             img[base_in + p] / 255.0)
    labels_out = bytes(lab[:n])

    img_path = out_dir / f"{split}_images.f32"
    lab_path = out_dir / f"{split}_labels.u8"
    img_path.write_bytes(images_out)
    lab_path.write_bytes(labels_out)
    return [f"{img_path.name} {sha256_bytes(images_out)}",
            f"{lab_path.name} {sha256_bytes(labels_out)}",
            f"{split} records {n} (of {count}), shape {rows}x{cols}"]


def main() -> None:
    ap = argparse.ArgumentParser(
        description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--train", type=int, default=2000,
                    help="training records (default 2000)")
    ap.add_argument("--test", type=int, default=1000,
                    help="test records (default 1000)")
    ap.add_argument("--source", type=Path, default=None,
                    help="directory holding local IDX .gz files (offline)")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cache = REPO / "temp" / "mnist_idx"

    lines = [f"generated by tools/mnist_subset.py "
             f"--train {args.train} --test {args.test}",
             f"source mirror {MIRROR} (upstream .gz SHA256 verified)",
             f"subset policy: first N records in file order"]
    for split, n in (("train", args.train), ("test", args.test)):
        img_name, lab_name = IDX_FILES[split]
        lines += [f"[{split}]"] + write_subset(
            split, n, fetch(img_name, cache, args.source),
            fetch(lab_name, cache, args.source), OUT_DIR)

    readme = OUT_DIR / "README.md"
    readme.write_text("# MNIST subset provenance\n\n```\n"
                      + "\n".join(lines) + "\n```\n", encoding="utf-8")
    print("\n".join(lines))
    print(f"\nwrote {OUT_DIR}")


if __name__ == "__main__":
    main()
