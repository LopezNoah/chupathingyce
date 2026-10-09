"""Build and run the bounded GLB reader tests without engine/map dependencies."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    flags = ["-std=c11", "-D_GNU_SOURCE", "-DGLB_READER_TEST", "-Wall", "-Wextra",
             "-Wpedantic", "-Wconversion", "-Wshadow", "-Wstrict-prototypes",
             "-Wmissing-prototypes", "-Werror", "-g"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="glb-reader-") as directory:
        output = Path(directory) / "test"
        subprocess.run([os.environ.get("CC", "clang"), *flags,
                        str(repo / "port/linux/src/glb_reader.c"),
                        str(repo / "tools/test_glb_reader.c"), "-lm", "-o", str(output)],
                       cwd=directory, check=True)
        subprocess.run([str(output), str(repo / "blender/forge_grid.glb")], check=True)


if __name__ == "__main__":
    main()
