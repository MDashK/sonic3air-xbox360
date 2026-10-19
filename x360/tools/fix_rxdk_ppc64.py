#!/usr/bin/env python3
"""Regenerate RXDK-360's translated XDK archives as ELFCLASS64 (ppc64).

The RXDK-360 installer (RxdkXdkUnpacker, release "latest" be16a94) still
translates the XDK .lib files to ELFCLASS32 / powerpc-unknown-xbox360, while
its clang toolset, libc.a and libcpp.a were moved to powerpc64-unknown-xbox360
(ELFCLASS64, ILP32-on-ppc64) upstream in late September 2026. ld.lld refuses to
mix the two ("... is incompatible with ..."), so no clang title links.

This script re-runs the translation with the upstream coff2elf.py in --ppc64
mode (vendored in rxdk_upstream/) and reassembles kernel_import.a for ppc64.
It also patches libc.a: adds the MSVC helper objects the release is missing, and
replaces memcpy/memmove/mempcpy with a version that never uses VMX on GPU
(physical) memory, which otherwise hangs D3D on real hardware.
The original ELF32 files are backed up once to lib\\xbox\\_elf32_backup.

    python -I fix_rxdk_ppc64.py [--rxdk E:\\RXDK-360] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COFF2ELF = os.path.join(HERE, "rxdk_upstream", "coff2elf.py")
TRIPLE = "powerpc64-unknown-xbox360"
RUNTIME_ARCHIVES = ("libc.a", "libcpp.a")
ELFCLASS64 = 2


def elf_class_of_archive(path):
	"""ELF class (1 = 32-bit, 2 = 64-bit) of the first ELF member in an ar archive."""
	# Large archives start with a symbol index of several MB, so scan the whole file
	with open(path, "rb") as f:
		data = f.read()
	pos = data.find(b"\x7fELF")
	return data[pos + 4] if pos >= 0 else 0


def translate(lib_dir, name):
	lib_path = os.path.join(lib_dir, name + ".lib")
	out_a = os.path.join(lib_dir, name + ".a")
	cmd = [sys.executable, "-I", COFF2ELF, "archive", lib_path, "-o", out_a, "--ppc64"]
	for runtime in RUNTIME_ARCHIVES:
		cmd += ["--runtime", os.path.join(lib_dir, runtime)]
	result = subprocess.run(cmd, capture_output=True, text=True)
	return name, result.returncode, (result.stdout + result.stderr).strip()


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--rxdk", default=os.environ.get("RXDK360", r"E:\RXDK-360"))
	parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
	args = parser.parse_args()

	lib_dir = os.path.join(args.rxdk, "lib", "xbox")
	clang_bin = os.path.join(args.rxdk, "bin", "clang", "bin")
	backup_dir = os.path.join(lib_dir, "_elf32_backup")

	for runtime in RUNTIME_ARCHIVES:
		if elf_class_of_archive(os.path.join(lib_dir, runtime)) != ELFCLASS64:
			sys.exit(f"{runtime} is not ELF64 - this script assumes the ppc64 runtime")

	# Every generated archive has a .lib of the same name, except the runtime and kernel_import
	names = sorted(f[:-2] for f in os.listdir(lib_dir)
				   if f.endswith(".a") and f not in RUNTIME_ARCHIVES and f != "kernel_import.a"
				   and os.path.exists(os.path.join(lib_dir, f[:-2] + ".lib")))

	# One-time backup of the original ELF32 outputs
	if not os.path.isdir(backup_dir):
		os.makedirs(backup_dir)
		for name in names + ["kernel_import"]:
			for ext in (".a", ".imports.json"):
				src = os.path.join(lib_dir, name + ext)
				if os.path.exists(src):
					shutil.copy2(src, backup_dir)
		print(f"Backed up originals to {backup_dir}")

	# LTCG libraries hold MSVC IL objects, so their translated archives contain no ELF at all (class 0)
	ltcg_only = [n for n in names if elf_class_of_archive(os.path.join(lib_dir, n + ".a")) == 0]
	names = [n for n in names if n not in ltcg_only]
	if ltcg_only:
		print(f"Skipping {len(ltcg_only)} archives without ELF objects (LTCG IL): {', '.join(ltcg_only)}")

	todo = [n for n in names if elf_class_of_archive(os.path.join(lib_dir, n + ".a")) != ELFCLASS64]
	print(f"{len(names)} XDK archives, {len(todo)} still ELF32 -> translating with {args.jobs} jobs")

	failures = []
	with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
		for name, code, output in pool.map(lambda n: translate(lib_dir, n), todo):
			status = "ok" if code == 0 else "FAILED"
			print(f"  [{status}] {name}")
			if code != 0:
				failures.append(name)
				print("    " + output.replace("\n", "\n    "))

	# kernel_import.a: the installer left its generated assembly next to it
	kernel_s = os.path.join(lib_dir, "kernel_import.s")
	kernel_o = os.path.join(lib_dir, "kernel_import.o")
	kernel_a = os.path.join(lib_dir, "kernel_import.a")
	if elf_class_of_archive(kernel_a) != ELFCLASS64:
		subprocess.run([os.path.join(clang_bin, "clang.exe"), "--target=" + TRIPLE, "-c", kernel_s, "-o", kernel_o], check=True)
		os.remove(kernel_a)
		subprocess.run([os.path.join(clang_bin, "llvm-ar.exe"), "rcs", kernel_a, kernel_o], check=True)
		print("  [ok] kernel_import (reassembled for ppc64)")

	wrong = [n for n in names + ["kernel_import"] if elf_class_of_archive(os.path.join(lib_dir, n + ".a")) != ELFCLASS64]
	if failures or wrong:
		sys.exit(f"Failed: {failures}; still not ELF64: {wrong}")
	print("All XDK archives are ELF64 now.")

	add_ms_glue_to_libc(lib_dir, clang_bin, backup_dir)
	replace_memcopy_in_libc(lib_dir, clang_bin, backup_dir)
	replace_file_io_in_libc(lib_dir, clang_bin, backup_dir)


# RXDK's libc opens files as "\??\game:\x" with no root directory. The console accepts it, but Xenia
# only resolves the XDK's own form ("game:\x" relative to RootDirectory (HANDLE)-3), so fopen / stat /
# opendir / std::filesystem all fail under Xenia. x360/runtime/rxdk_patched holds upstream's fileio.c
# and dirio.c (commit in UPSTREAM_COMMIT.txt) with that one change; compile them with upstream's libc
# flags (tools/build_libc.py) and swap them in under the same member names.
FILE_IO_DIR = os.path.join(os.path.dirname(HERE), "runtime", "rxdk_patched")
FILE_IO_MEMBERS = {"fileio.c": "xbox_fileio.o", "dirio.c": "xbox_dirio.o"}
FILE_IO_MARKER = b"s3air xdk-paths 1"

def replace_file_io_in_libc(lib_dir, clang_bin, backup_dir):
	libc = os.path.join(lib_dir, "libc.a")
	with open(libc, "rb") as f:
		if FILE_IO_MARKER in f.read():
			print("libc.a already has the XDK-style file paths.")
			return

	clang_root = os.path.dirname(clang_bin)
	work_dir = os.path.join(backup_dir, "fileio_work")
	os.makedirs(work_dir, exist_ok=True)
	objects = []
	for source, member in FILE_IO_MEMBERS.items():
		obj = os.path.join(work_dir, member)
		subprocess.run([os.path.join(clang_bin, "clang.exe"), "--target=" + TRIPLE, "-std=c17", "-O2", "-ffreestanding",
						"-ffunction-sections", "-fdata-sections", "-fshort-wchar", "-fno-stack-protector",
						"-fno-zero-initialized-in-bss", "-fno-sanitize=all", "-fno-builtin", "-Wno-everything",
						"-D__Picolibc__", "-D__TINY_STDIO", "-include", "picolibc.h",
						"-I" + os.path.join(clang_root, "include", "config"),
						"-I" + os.path.join(clang_root, "include", "picolibc"),
						"-I" + FILE_IO_DIR,
						"-c", os.path.join(FILE_IO_DIR, source), "-o", obj], check=True)
		objects.append(obj)
	subprocess.run([os.path.join(clang_bin, "llvm-ar.exe"), "rs", libc] + objects, check=True)
	print(f"Replaced {', '.join(FILE_IO_MEMBERS.values())} in libc.a with the XDK-style path versions")


# RXDK's memcpy/memmove/mempcpy use the VMX copy for any destination at PASSIVE_LEVEL, including
# D3D's physical/write-combined GPU memory, which hangs the GPU on real hardware (see
# x360/runtime/memcopy_safe.c). Swap in our version under the same member name.
MEMCOPY_SOURCE = os.path.join(os.path.dirname(HERE), "runtime", "memcopy_safe.c")
MEMCOPY_MEMBER = "xbox_memcopy_irql.o"
MEMCOPY_MARKER = b"s3air memcopy_safe 1"

def replace_memcopy_in_libc(lib_dir, clang_bin, backup_dir):
	libc = os.path.join(lib_dir, "libc.a")
	with open(libc, "rb") as f:
		if MEMCOPY_MARKER in f.read():
			print("libc.a already has the GPU-safe memcpy/memmove/mempcpy.")
			return

	work_dir = os.path.join(backup_dir, "memcopy_work")
	os.makedirs(work_dir, exist_ok=True)
	obj = os.path.join(work_dir, MEMCOPY_MEMBER)
	subprocess.run([os.path.join(clang_bin, "clang.exe"), "--target=" + TRIPLE, "-O2",
					"-mno-altivec", "-msoft-float", "-ffreestanding", "-fno-builtin",
					"-ffunction-sections", "-fdata-sections", "-c", MEMCOPY_SOURCE, "-o", obj], check=True)
	# 'r' replaces the existing member of the same name
	subprocess.run([os.path.join(clang_bin, "llvm-ar.exe"), "rs", libc, obj], check=True)
	print(f"Replaced {MEMCOPY_MEMBER} in libc.a with the GPU-safe version ({os.path.basename(MEMCOPY_SOURCE)})")


# Leaf helpers that MS-compiled XDK libs call (register save/restore, stack probe,
# uint64->double). Upstream's tools/build_libc.py bundles these members of the
# translated libcMT into libc.a, but the released libc.a was built without
# libcMT available, so they are missing and every XDK lib reference fails to link.
MS_GLUE_MEMBERS = ("crtgpr.o", "crtfpr.o", "crtvmx.o", "u64tod.o", "chkstk.o", "jmpuwind.o")

def add_ms_glue_to_libc(lib_dir, clang_bin, backup_dir):
	ar = os.path.join(clang_bin, "llvm-ar.exe")
	libc = os.path.join(lib_dir, "libc.a")
	existing = subprocess.run([ar, "t", libc], capture_output=True, text=True, check=True).stdout.split()
	missing = [m for m in MS_GLUE_MEMBERS if "msglue_" + m not in existing]
	if not missing:
		print("libc.a already contains the MS glue helpers.")
		return

	backup_libc = os.path.join(backup_dir, "libc.a.orig")
	if not os.path.exists(backup_libc):
		shutil.copy2(libc, backup_libc)

	work_dir = os.path.join(backup_dir, "msglue_work")
	os.makedirs(work_dir, exist_ok=True)
	subprocess.run([ar, "x", "--output=" + work_dir, os.path.join(lib_dir, "libcMT.a")] + missing, check=True)
	renamed = []
	for member in missing:
		target = os.path.join(work_dir, "msglue_" + member)
		os.replace(os.path.join(work_dir, member), target)
		renamed.append(target)
	subprocess.run([ar, "rs", libc] + renamed, check=True)
	print(f"Added {len(renamed)} MS glue helpers to libc.a ({', '.join(missing)})")


if __name__ == "__main__":
	main()
