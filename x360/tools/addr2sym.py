#!/usr/bin/env python3
"""Map Xbox 360 code addresses to function names using the linked ELF that the
RXDK clang toolset keeps next to the .xex (RxdkClangDebug=true).

    python -I addr2sym.py <default.elf> <address> [address ...]
    python -I addr2sym.py <default.elf> --log <cpp17hello.log>     (every 0x82xxxxxx in a log)

Addresses are taken as-is: the XEX is loaded at the ELF's link addresses (base 0x82000000).
"""
import argparse
import bisect
import re
import struct


def load_functions(path):
	elf = open(path, "rb").read()
	if elf[:4] != b"\x7fELF":
		raise SystemExit(f"{path} is not an ELF file")
	is64 = elf[4] == 2
	e = ">" if elf[5] == 2 else "<"
	if is64:
		shoff, = struct.unpack(e + "Q", elf[0x28:0x30])
		shentsize, shnum = struct.unpack(e + "HH", elf[0x3A:0x3E])
	else:
		shoff, = struct.unpack(e + "I", elf[0x20:0x24])
		shentsize, shnum = struct.unpack(e + "HH", elf[0x2E:0x32])

	sections = []
	for i in range(shnum):
		raw = elf[shoff + i * shentsize: shoff + (i + 1) * shentsize]
		fmt = "IIQQQQIIQQ" if is64 else "IIIIIIIIII"
		sections.append(struct.unpack(e + fmt, raw))

	symbols = []
	for sec in sections:
		sh_type, offset, size, link, entsize = sec[1], sec[4], sec[5], sec[6], sec[9]
		if sh_type != 2:	# SHT_SYMTAB
			continue
		str_off, str_size = sections[link][4], sections[link][5]
		strtab = elf[str_off:str_off + str_size]
		for j in range(1, size // entsize):
			raw = elf[offset + j * entsize: offset + (j + 1) * entsize]
			if is64:
				st_name, st_info, _o, shndx, value, sym_size = struct.unpack(e + "IBBHQQ", raw)
			else:
				st_name, value, sym_size, st_info, _o, shndx = struct.unpack(e + "IIIBBH", raw)
			sym_type = st_info & 0xF
			if shndx == 0 or sym_type not in (0, 2):	# defined NOTYPE/FUNC only
				continue
			name = strtab[st_name:strtab.find(b"\0", st_name)].decode("ascii", "replace")
			if name and not name.startswith(".L"):
				symbols.append((value, sym_size, name))
	symbols.sort()
	return symbols


def demangle_hint(name):
	return name	# Itanium names (_Z...) are left as-is; paste them into a demangler if needed


def lookup(symbols, starts, address):
	index = bisect.bisect_right(starts, address) - 1
	if index < 0:
		return "?"
	value, size, name = symbols[index]
	offset = address - value
	if size and offset >= size:
		return f"? (after {demangle_hint(name)}+0x{offset:X})"
	return f"{demangle_hint(name)}+0x{offset:X}"


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("elf")
	parser.add_argument("addresses", nargs="*")
	parser.add_argument("--log", help="symbolize every 0x8xxxxxxx address found in this log")
	args = parser.parse_args()

	symbols = load_functions(args.elf)
	starts = [s[0] for s in symbols]

	addresses = [int(a, 16) for a in args.addresses]
	if args.log:
		text = open(args.log, encoding="utf-8", errors="replace").read()
		addresses += [int(m, 16) for m in re.findall(r"0x(8[0-9A-Fa-f]{7})", text)]

	seen = set()
	for address in addresses:
		if address in seen:
			continue
		seen.add(address)
		print(f"0x{address:08X}  {lookup(symbols, starts, address)}")


if __name__ == "__main__":
	main()
