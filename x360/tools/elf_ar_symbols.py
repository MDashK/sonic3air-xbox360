#!/usr/bin/env python3
"""List defined/undefined symbols of the ELF members of an ar archive (no binutils needed).

    python -I elf_ar_symbols.py <archive.a> [--undefined] [--defined] [--member substring]

Supports ELFCLASS32 and ELFCLASS64, big- and little-endian.
"""
import argparse
import struct


def read_ar_members(data):
	pos = 8
	long_names = b""
	while pos + 60 <= len(data):
		header = data[pos:pos + 60]
		name = header[0:16].decode("ascii", "replace").rstrip()
		size = int(header[48:58].decode("ascii").strip())
		body = data[pos + 60:pos + 60 + size]
		pos += 60 + size + (size & 1)
		if name == "//":
			long_names = body
			continue
		if name in ("/", "/SYM64/"):
			continue
		if name.startswith("/") and name[1:].isdigit():
			start = int(name[1:])
			end = long_names.find(b"/\n", start)
			name = long_names[start:end].decode("ascii", "replace")
		yield name.rstrip("/"), body


def elf_symbols(elf):
	if elf[:4] != b"\x7fELF":
		return []
	is64 = elf[4] == 2
	endian = ">" if elf[5] == 2 else "<"
	if is64:
		shoff, = struct.unpack(endian + "Q", elf[0x28:0x30])
		shentsize, shnum = struct.unpack(endian + "HH", elf[0x3A:0x3E])
	else:
		shoff, = struct.unpack(endian + "I", elf[0x20:0x24])
		shentsize, shnum = struct.unpack(endian + "HH", elf[0x2E:0x32])

	sections = []
	for i in range(shnum):
		raw = elf[shoff + i * shentsize: shoff + (i + 1) * shentsize]
		if is64:
			_name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize = struct.unpack(endian + "IIQQQQIIQQ", raw)
		else:
			_name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize = struct.unpack(endian + "IIIIIIIIII", raw)
		sections.append((sh_type, offset, size, link, entsize))

	result = []
	for sh_type, offset, size, link, entsize in sections:
		if sh_type != 2:		# SHT_SYMTAB
			continue
		_t, str_off, str_size, _l, _e = sections[link]
		strtab = elf[str_off:str_off + str_size]
		for j in range(1, size // entsize):
			raw = elf[offset + j * entsize: offset + (j + 1) * entsize]
			if is64:
				st_name, st_info, _other, st_shndx, _value, _size = struct.unpack(endian + "IBBHQQ", raw)
			else:
				st_name, _value, _size, st_info, _other, st_shndx = struct.unpack(endian + "IIIBBH", raw)
			bind = st_info >> 4
			if bind == 0:		# STB_LOCAL
				continue
			name = strtab[st_name:strtab.find(b"\0", st_name)].decode("ascii", "replace")
			result.append((name, st_shndx == 0))
	return result


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("archive")
	parser.add_argument("--undefined", action="store_true")
	parser.add_argument("--defined", action="store_true")
	parser.add_argument("--member", default="")
	args = parser.parse_args()

	data = open(args.archive, "rb").read()
	for member, body in read_ar_members(data):
		if args.member not in member:
			continue
		for name, undefined in elf_symbols(body):
			if (args.undefined and not undefined) or (args.defined and undefined):
				continue
			print(f"{member}\t{'U' if undefined else 'D'}\t{name}")


if __name__ == "__main__":
	main()
