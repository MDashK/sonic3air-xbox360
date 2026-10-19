#!/usr/bin/env python3
"""Translate the XDK's PPC COFF import/static libraries to PPC32 ELF archives.

The hybrid RXDK-360 toolchain reuses Microsoft's shipped `lib\\xbox\\*.lib`
rather than reimplementing them. Those are MS archives of big-endian PowerPC
COFF objects (machine 0x01F2), a format lld cannot read. This rewrites each
object as a PPC32 big-endian ELF and repacks the archive as a System V `.a`
that lld links natively.

Only what the shipped libraries actually use is handled -- the relocation set
was measured at five real types (ADDR32, REL24, REFHI, REFLO, PAIR) plus the
droppable debug-only SECTION/SECREL. Everything else is reported rather than
guessed at, so an unexpected construct surfaces loudly instead of miscompiling.

This module is being built in stages. Right now it parses and reports; ELF
emission follows once the input structure is confirmed against dumpbin on the
real libraries.

Usage:
    python tools/coff2elf.py dump   <lib>            structure of the first object
    python tools/coff2elf.py survey <lib> [lib ...]  tally reloc/section/class use
"""
import argparse
import struct
import sys
from collections import Counter


# ---- Microsoft archive (.lib) ----------------------------------------------

ARCHIVE_MAGIC = b"!<arch>\n"


class Member:
    __slots__ = ("name", "data", "offset")

    def __init__(self, name, data, offset):
        self.name = name
        self.data = data
        self.offset = offset


def read_archive(blob):
    """Yield Member objects for every archive member, in file order.

    The two linker members (both named "/") and the longnames member ("//")
    are yielded too; callers skip them by name. Long member names are resolved
    through the longnames member per the COFF archive spec.
    """
    if blob[:8] != ARCHIVE_MAGIC:
        sys.exit("not a Microsoft archive: bad !<arch> magic")
    pos = 8
    longnames = b""
    members = []
    raw = []
    while pos + 60 <= len(blob):
        header = blob[pos:pos + 60]
        if header[58:60] != b"`\n":
            sys.exit(f"malformed archive header at 0x{pos:X}")
        name = header[0:16].decode("ascii", "replace").rstrip()
        size = int(header[48:58].decode("ascii").strip())
        data_off = pos + 60
        data = blob[data_off:data_off + size]
        raw.append((name, data, data_off))
        pos = data_off + size + (size & 1)          # members are 2-byte aligned

    for i, (name, data, off) in enumerate(raw):
        if i == 2 and name == "//":
            longnames = data
            continue
        yield Member(name, data, off), longnames


def member_name(name, longnames):
    """Resolve a member's real name: '/123' indexes the longnames member."""
    if name.startswith("/") and name[1:].isdigit():
        start = int(name[1:])
        end = longnames.find(b"\0", start)
        return longnames[start:end].decode("ascii", "replace")
    return name.rstrip("/")


# ---- PPC COFF object --------------------------------------------------------

IMAGE_FILE_MACHINE_POWERPCBE = 0x01F2

# IMAGE_REL_PPC_* relocation types, from winnt.h. The numbering was confirmed
# against real objects: the debug sections carry SECREL/SECTION, which show up
# as 0x0B/0x0C, fixing the high types (REFHI/REFLO/PAIR) at 0x10-0x12.
REL_TYPES = {
    0x0000: "ABSOLUTE", 0x0001: "ADDR64", 0x0002: "ADDR32", 0x0003: "ADDR24",
    0x0004: "ADDR16", 0x0005: "ADDR14", 0x0006: "REL24", 0x0007: "REL14",
    0x000A: "ADDR32NB", 0x000B: "SECREL", 0x000C: "SECTION", 0x000F: "SECREL16",
    0x0010: "REFHI", 0x0011: "REFLO", 0x0012: "PAIR", 0x0013: "SECRELLO",
    0x0015: "GPREL", 0x0016: "TOCREL16", 0x0017: "TOCREL14",
}

# The low bits (0x60) of the type word are a "restart" field on some MS PPC
# relocations; mask them off before lookup.
REL_TYPE_MASK = 0x00FF

STORAGE_CLASS = {
    2: "EXTERNAL", 3: "STATIC", 6: "LABEL", 103: "FILE", 105: "SECTION",
    101: "FUNCTION", 100: "BLOCK", 104: "STRTAG", 3: "STATIC",
}


class Section:
    __slots__ = ("name", "vsize", "vaddr", "rawsize", "rawptr", "relptr",
                 "nreloc", "flags", "data", "relocs")


class Symbol:
    __slots__ = ("name", "value", "secnum", "type", "cls", "naux", "aux")


class CoffObject:
    """A parsed big-endian PowerPC COFF object.

    Every scalar in the COFF headers is little-endian regardless of the target
    byte order; only the section *contents* are big-endian PPC. That split is
    the usual source of COFF bugs, so it is called out at each unpack.
    """

    def __init__(self, blob):
        self.blob = blob
        (self.machine, self.nsections, self.timestamp, self.symptr,
         self.nsymbols, self.optsize, self.characteristics) = struct.unpack_from(
            "<HHIIIHH", blob, 0)

        self.sections = []
        off = 20 + self.optsize
        for _ in range(self.nsections):
            s = Section()
            (name, s.vsize, s.vaddr, s.rawsize, s.rawptr, s.relptr,
             _lineptr, s.nreloc, _nline, s.flags) = struct.unpack_from(
                "<8sIIIIIIHHI", blob, off)
            s.name = self._section_name(name)
            s.data = blob[s.rawptr:s.rawptr + s.rawsize] if s.rawptr else b""
            s.relocs = self._read_relocs(s)
            self.sections.append(s)
            off += 40

        self.symbols = self._read_symbols()

    def _strtab(self):
        # the string table follows the symbol table; its first 4 bytes are its
        # own size, and symbol/section name offsets are measured from its start
        base = self.symptr + self.nsymbols * 18
        return base

    def _section_name(self, raw):
        if raw[0:1] == b"/":                       # "/123" -> offset into strtab
            offset = int(raw[1:].split(b"\0")[0].decode("ascii"))
            base = self._strtab()
            end = self.blob.find(b"\0", base + offset)
            return self.blob[base + offset:end].decode("ascii", "replace")
        return raw.split(b"\0")[0].decode("ascii", "replace")

    def _read_relocs(self, s):
        out = []
        for i in range(s.nreloc):
            o = s.relptr + i * 10
            vaddr, symidx, typ = struct.unpack_from("<IIH", self.blob, o)
            out.append((vaddr, symidx, typ))
        return out

    def _sym_name(self, raw):
        if raw[0:4] == b"\0\0\0\0":
            offset, = struct.unpack_from("<I", raw, 4)
            base = self._strtab()
            end = self.blob.find(b"\0", base + offset)
            return self.blob[base + offset:end].decode("ascii", "replace")
        return raw.split(b"\0")[0].decode("ascii", "replace")

    def _read_symbols(self):
        out = []
        i = 0
        while i < self.nsymbols:
            o = self.symptr + i * 18
            raw = self.blob[o:o + 18]
            sym = Symbol()
            sym.name = self._sym_name(raw[0:8])
            sym.value, sym.secnum, sym.type, sym.cls, sym.naux = struct.unpack_from(
                "<iHHBB", raw, 8)
            sym.aux = [self.blob[o + 18 + k * 18:o + 18 + (k + 1) * 18]
                       for k in range(sym.naux)]
            out.append(sym)
            i += 1 + sym.naux                       # aux records share the index space
        return out


# ---- PPC32 big-endian ELF emission ------------------------------------------

# ELF constants
ET_REL = 1
EM_PPC = 20
EM_PPC64 = 21
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHT_GROUP = 17
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR, SHF_GROUP = 0x1, 0x2, 0x4, 0x200
GRP_COMDAT = 0x1
IMAGE_COMDAT_SELECT_ASSOCIATIVE = 5
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION = 0, 1, 2, 3
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2

# PPC ELF relocation types
R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA, R_PPC_REL24 = \
    1, 4, 5, 6, 10

# COFF section characteristics
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080
IMAGE_SCN_LNK_INFO = 0x00000200            # .drectve and other linker-only info
IMAGE_SCN_LNK_REMOVE = 0x00000800          # not carried into the image
IMAGE_SCN_LNK_COMDAT = 0x00001000
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000     # debug etc.: dropped before load
IMAGE_SCN_MEM_WRITE = 0x80000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000

# COFF storage classes
IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SYM_CLASS_STATIC = 3
IMAGE_SYM_CLASS_LABEL = 6
IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105

# relocation types handled or deliberately dropped
_DROP_RELOCS = {0x0B, 0x0C}          # SECREL, SECTION -- debug only
_IMPORT_RELOCS = {0x0A}              # ADDR32NB -- import descriptors, packer's job


def _keep_section(name, flags=0):
    """The ELF section a COFF section is carried into, or None to drop it.

    Kept by CONTENT rather than a per-name allow-list: an executable section
    becomes .text, uninitialised data .bss, initialised data .data (writable)
    or .rodata (read-only). This preserves the MS libraries' many Xbox-specific
    named data/code sections -- .XBLD$V (a lib's <Lib>BuildNumber), .XBMOVIE$*
    (movie-capture globals), and any others a library defines and references --
    without chasing each name, while dropping the host-only metadata families
    (debug, linker directives, import/export/resource tables). A few names keep
    a canonical target: .rdata -> .rodata, the .CRT$X* ctor/dtor tables keep
    their grouped name for the linker script, and the unwind tables keep their
    own .pdata/.xdata so the EH machinery still finds them.
    """
    if name == ".rdata" or name.startswith(".rdata$"):
        return ".rodata"
    if name.startswith(".CRT$"):
        return name
    base = name.split("$")[0]
    # host-only metadata never carried into the image. Imports come from the
    # short-import members / gen_import_stubs, not .idata; debug and .drectve
    # are compiler/linker bookkeeping.
    if base in (".debug", ".drectve", ".idata", ".didat", ".edata", ".rsrc",
                ".sxdata", ".gfids", ".giats", ".gljmp"):
        return None
    # .XBLD$W is the compiler build stamp (__C1_<ver>/__C2_<ver>), emitted into
    # every object by a given cl.exe -- unreferenced metadata that would collide
    # as duplicate symbols across libraries. Drop it (unlike .XBLD$V, whose
    # <Lib>BuildNumber word is referenced and kept by the content rule below).
    if name == ".XBLD$W":
        return None
    # .drectve / other linker-only info, and sections explicitly marked remove.
    # Note MEM_DISCARDABLE is NOT a drop signal: MS marks .XBLD$V (the referenced
    # <Lib>BuildNumber word) discardable, and we need the symbols it defines --
    # anything genuinely unused is removed by --gc-sections instead.
    if flags & (IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE):
        return None
    # the standard families keep their canonical name (unwind tables must stay
    # .pdata/.xdata rather than fold into .rodata).
    if base in (".text", ".data", ".bss", ".pdata", ".xdata"):
        return base
    # everything else that carries real content is kept by what it holds.
    if flags & IMAGE_SCN_CNT_CODE:
        return ".text"
    if flags & IMAGE_SCN_CNT_UNINITIALIZED_DATA:
        return ".bss"
    if flags & IMAGE_SCN_CNT_INITIALIZED_DATA:
        return ".data" if (flags & IMAGE_SCN_MEM_WRITE) else ".rodata"
    return None                       # no content (or unknown flags) -> drop


class StrTab:
    """An ELF string table: deduplicated, first byte is the empty string."""

    def __init__(self):
        self.buf = bytearray(b"\0")
        self.offsets = {"": 0}

    def add(self, s):
        if s in self.offsets:
            return self.offsets[s]
        off = len(self.buf)
        self.offsets[s] = off
        self.buf += s.encode("utf-8") + b"\0"
        return off


def strong_noncomdat_globals(obj):
    """Names this object defines STRONG in a non-COMDAT section -- an out-of-line
    definition that COMDAT ("pick any") copies elsewhere must yield to."""
    out = set()
    for _, sym in _enumerate_coff_syms(obj):
        if (sym.cls == IMAGE_SYM_CLASS_EXTERNAL and 0 < sym.secnum <= len(obj.sections)
                and not (obj.sections[sym.secnum - 1].flags & IMAGE_SCN_LNK_COMDAT)):
            out.add(sym.name)
    return out


def _keep_code_signature_strong(signame, external_strong):
    """A CODE COMDAT signature is kept STRONG only when weakening it would break
    resolution, NOT by default. Two cases need strong:

      * The name has a real strong out-of-line definition in ANOTHER kept archive
        (external_strong, e.g. picolibc's `powf` in libc.a). Keeping the XDK
        COMDAT copy strong lets --allow-multiple-definition pick the runtime's
        without discarding the XDK group that the archive's own code references.
      * A vtable deleting-destructor thunk (`??_E`/`??_G`): a COMDAT group that may
        be referenced only from another object, which a weak signature could let
        --gc-sections drop.

    Every other code signature (a pure header-inline like FXL's fxl.inl family --
    FXLEffect_GetParameterHandle et al.) is weakened, so when a title emits its own
    linkonce_odr copy the COMDAT dedup keeps the title's group and resolution
    follows it, instead of resolving to the strong copy in the discarded XDK group
    ("relocation refers to a symbol in a discarded section")."""
    return (signame in external_strong
            or signame.startswith("??_E") or signame.startswith("??_G"))


def coff_to_elf(obj, warn=print, noncomdat_strong=frozenset(),
                external_strong=frozenset(), elf64=False):
    """Translate one parsed CoffObject to PPC big-endian ELF bytes.

    elf64=False emits ELFCLASS32/EM_PPC (the legacy powerpc-unknown-xbox360
    pipeline); elf64=True emits ELFCLASS64/EM_PPC64 with the same 32-bit content
    (ILP32-on-ppc64) to match the powerpc64-unknown-xbox360 clang.

    Each kept COFF section becomes its own ELF section in the same order, so a
    symbol's 1-based section number and a relocation's section-relative offset
    both carry across unchanged.
    """
    # 1. decide which COFF sections survive, in order, and assign ELF indices.
    #    ELF layout: [0]=null, then kept sections, then .symtab .strtab .shstrtab,
    #    then one .rela.* per kept section that has real relocations.
    kept = []                          # (coff_index, elf_name, Section)
    coff_to_elfshndx = {}              # 1-based COFF secnum -> ELF section index
    for ci, s in enumerate(obj.sections, start=1):
        elf_name = _keep_section(s.name, s.flags)
        if elf_name is None:
            continue
        coff_to_elfshndx[ci] = 1 + len(kept)
        kept.append((ci, elf_name, s))

    # 1b. COMDAT selection per kept COMDAT section, read from its section symbol's
    #     aux record (Auxiliary Format 5): Selection@14, associated Number@12. A
    #     COMDAT section becomes a real ELF section group below (not the old weak
    #     hack), so its defining symbols stay strong and lld dedups whole groups.
    sec_selection = {}                 # coff secnum -> (selection, assoc_number)
    for _, sym in _enumerate_coff_syms(obj):
        if (sym.cls == IMAGE_SYM_CLASS_STATIC and sym.naux >= 1
                and sym.secnum in coff_to_elfshndx
                and (obj.sections[sym.secnum - 1].flags & IMAGE_SCN_LNK_COMDAT)):
            aux = sym.aux[0]
            if len(aux) >= 15:
                number = struct.unpack_from("<H", aux, 12)[0]
                sec_selection.setdefault(sym.secnum, (aux[14], number))

    # 2. symbols. Emit locals first (ELF requires it): a STT_SECTION symbol per
    #    kept section, then local COFF symbols, then globals. Build a map from
    #    COFF symbol index to ELF symbol index for the relocations.
    strtab = StrTab()
    elf_syms = [(0, 0, 0, 0, 0, SHN_UNDEF)]   # index 0 is the null symbol
    sym_name_off = [0]

    section_sym_of = {}                # ELF section index -> its STT_SECTION sym
    for elf_idx, (ci, name, s) in enumerate(kept, start=1):
        section_sym_of[elf_idx] = len(elf_syms)
        elf_syms.append((0, 0, 0, (STB_LOCAL << 4) | STT_SECTION, 0, elf_idx))
        sym_name_off.append(0)

    # For resolving weak-external fall-backs (below): a COFF index -> Symbol map
    # (the aux TagIndex names the default symbol by index), and a lookup of names
    # that are actually *defined* in a kept section of this object.
    coff_by_index = {ci: s for ci, s in _enumerate_coff_syms(obj)}
    defined_here = {}
    for _ci, s in _enumerate_coff_syms(obj):
        if s.secnum in coff_to_elfshndx:
            defined_here.setdefault(s.name, s)

    coff_to_elfsym = {}
    # locals (STATIC / LABEL) first
    for pass_globals in (False, True):
        for csi, sym in _enumerate_coff_syms(obj):
            # A COFF weak external (IMAGE_SYM_CLASS_WEAK_EXTERNAL) is an
            # externally-visible symbol -- MSVC emits these for inline functions
            # and the `??_E` vtable deleting-dtor thunks: "use a strong def if one
            # is linked, else fall back". It is NOT a static local, so it must go
            # in the global pass and be emitted STB_WEAK (see below); treating it
            # as local (cls != EXTERNAL) made references unresolvable by a real
            # global definition.
            is_weak_ext = sym.cls == IMAGE_SYM_CLASS_WEAK_EXTERNAL
            is_global = sym.cls == IMAGE_SYM_CLASS_EXTERNAL or is_weak_ext
            if is_global != pass_globals:
                continue
            # a STATIC symbol whose name is a kept section is that section's
            # definition -- fold it onto the ELF section symbol
            if (sym.cls == IMAGE_SYM_CLASS_STATIC and sym.secnum in coff_to_elfshndx
                    and _keep_section(sym.name,
                                      obj.sections[sym.secnum - 1].flags) is not None
                    and sym.name.split("$")[0] == obj.sections[sym.secnum - 1].name.split("$")[0]):
                coff_to_elfsym[csi] = section_sym_of[coff_to_elfshndx[sym.secnum]]
                continue
            # a section symbol for a dropped section (.debug$S, .drectve, ...):
            # nothing kept references it, so leave it out rather than emit a
            # bare undefined name
            if (sym.cls == IMAGE_SYM_CLASS_STATIC and sym.name.startswith(".")
                    and sym.secnum not in coff_to_elfshndx):
                continue

            bind = STB_GLOBAL if is_global else STB_LOCAL
            if is_weak_ext:                          # COFF weak external -> ELF weak
                bind = STB_WEAK
            size = 0
            if sym.secnum == 0:                     # undefined / common / weak
                shndx, value, styp = SHN_UNDEF, 0, STT_NOTYPE
                # A COFF COMMON symbol is an uninitialised tentative definition:
                # class EXTERNAL, no section, and value = the size in bytes. ELF
                # expresses it as SHN_COMMON (st_size = size, st_value =
                # alignment); the linker allocates it in .bss and merges the many
                # objects that legally declare the same common. Without this it
                # stayed a plain undefined and never resolved.
                if sym.cls == IMAGE_SYM_CLASS_EXTERNAL and sym.value > 0:
                    size = sym.value
                    align = 1
                    while align * 2 <= size and align < 16:
                        align *= 2
                    shndx, value, styp = SHN_COMMON, align, STT_OBJECT
                # A weak external is "use a strong def if linked, ELSE this
                # default symbol" (aux[0] TagIndex names the default; MSVC uses
                # it for the `??_E` vector deleting-dtor thunks, whose default is
                # the class's `??_G` scalar deleting dtor). Leaving it undefined
                # makes it resolve to 0 -- fine until such a thunk actually sits
                # in a vtable slot that gets called (e.g. std::locale facet
                # teardown), which then branches to 0. When the default is
                # defined in THIS object (the object that carries the vtable also
                # defines its `??_G`), emit the weak external as a weak ALIAS of
                # that default so the slot points at real code.
                if is_weak_ext and sym.naux >= 1 and len(sym.aux[0]) >= 4:
                    tagidx = struct.unpack_from("<I", sym.aux[0], 0)[0]
                    dflt = coff_by_index.get(tagidx)
                    target = defined_here.get(dflt.name) if dflt else None
                    if target is not None:
                        tflags = obj.sections[target.secnum - 1].flags
                        shndx = coff_to_elfshndx[target.secnum]
                        value = target.value
                        styp = (STT_FUNC if (tflags & IMAGE_SCN_MEM_EXECUTE)
                                else STT_OBJECT)
            elif sym.secnum == 0xFFFF or sym.secnum == 0xFFFFFFFF:
                shndx, value, styp = SHN_ABS, sym.value, STT_NOTYPE
            elif sym.secnum in coff_to_elfshndx:
                shndx = coff_to_elfshndx[sym.secnum]
                value = sym.value
                sflags = obj.sections[sym.secnum - 1].flags
                styp = STT_FUNC if (sflags & IMAGE_SCN_MEM_EXECUTE) else STT_OBJECT
                # a symbol in a COMDAT section (the MS ".text$"/".rdata$" groups
                # the CRT is full of): many objects legally define the same
                # symbol by name. When we can wrap the section in a real ELF
                # section group (below), keep the symbol STRONG and let lld dedup
                # whole groups -- this preserves the definition through
                # --gc-sections, unlike a bare weak symbol whose section GC can
                # drop out from under a live reference. Only fall back to STB_WEAK
                # for a COMDAT section we could not group (no section symbol/aux).
                if is_global and (sflags & IMAGE_SCN_LNK_COMDAT) \
                        and sym.secnum not in sec_selection:
                    bind = STB_WEAK
            else:
                # symbol in a dropped section (debug etc.) -- keep as a name only
                shndx, value, styp = SHN_UNDEF, 0, STT_NOTYPE
            coff_to_elfsym[csi] = len(elf_syms)
            elf_syms.append((strtab.add(sym.name), value, size, (bind << 4) | styp, 0, shndx))
            sym_name_off.append(0)

    # .symtab sh_info must be the index of the first non-local symbol -- weak
    # counts as non-local too, so test bind != LOCAL rather than == GLOBAL.
    first_global = next((i for i, s in enumerate(elf_syms)
                         if (s[3] >> 4) != STB_LOCAL), len(elf_syms))

    # 3. relocations per kept section
    rela = {}                          # elf section index -> list of (off, sym, type, addend)
    for elf_idx, (ci, name, s) in enumerate(kept, start=1):
        entries = []
        i = 0
        while i < len(s.relocs):
            va, symidx, typ = s.relocs[i]
            tt = typ & 0xFF
            if tt in _DROP_RELOCS:
                i += 1
                continue
            if tt in _IMPORT_RELOCS:
                i += 1               # ADDR32NB -> handled by the import manifest
                continue
            elfsym = coff_to_elfsym.get(symidx)
            if elfsym is None:
                warn(f"  reloc at 0x{va:X} targets unmapped symbol {symidx}")
                i += 1
                continue
            site = s.data[va:va + 4]
            inplace = struct.unpack(">I", site)[0] if len(site) == 4 else 0
            if tt == 0x02:                                  # ADDR32
                entries.append((va, elfsym, R_PPC_ADDR32, inplace))
            elif tt == 0x06:                                # REL24
                # The branch's 24-bit LI field holds (intended addend - P) as a
                # compile-time placeholder -- with the section based at 0, the
                # only PC-relative value it can encode. ELF RELA computes
                # S + A - P, so recover A = LI + P (P is this reloc's offset).
                li = inplace & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                addend = li + va
                if addend != 0:
                    warn(f"  non-zero REL24 addend {addend} at 0x{va:X}")
                entries.append((va, elfsym, R_PPC_REL24, addend))
            elif tt in (0x10, 0x11):                         # REFHI / REFLO (+ PAIR)
                pair_low = 0
                if i + 1 < len(s.relocs) and (s.relocs[i + 1][2] & 0xFF) == 0x12:
                    pair_low = s.relocs[i + 1][1]
                    i += 1
                imm = inplace & 0xFFFF
                addend = ((imm << 16) | (pair_low & 0xFFFF)) if tt == 0x10 else \
                         ((pair_low << 16) | imm)
                if addend & 0x80000000:
                    addend -= 0x100000000
                if addend != 0:
                    warn(f"  non-zero REFHI/REFLO addend 0x{addend:X} at 0x{va:X}"
                         f" -- check the split convention")
                rtype = R_PPC_ADDR16_HA if tt == 0x10 else R_PPC_ADDR16_LO
                # PPC ELF's 16-bit relocations modify the immediate halfword, so
                # r_offset points two bytes into the big-endian instruction --
                # where COFF REFHI/REFLO point at the instruction start.
                entries.append((va + 2, elfsym, rtype, addend))
            else:
                warn(f"  unhandled relocation type 0x{tt:X} at 0x{va:X}")
            i += 1
        if entries:
            rela[elf_idx] = entries

    # 4. COMDAT section groups. Signature = the first external symbol defined in
    #    the section (the name lld dedups on); an ASSOCIATIVE section folds into
    #    its parent's group (kept/discarded together with it).
    sec_sig = {}                       # coff secnum -> elf sym index (signature)
    sec_signame = {}                   # coff secnum -> signature symbol name
    for csi, sym in _enumerate_coff_syms(obj):
        if (sym.cls == IMAGE_SYM_CLASS_EXTERNAL and sym.secnum in sec_selection
                and csi in coff_to_elfsym):
            if sym.secnum not in sec_sig:
                sec_sig[sym.secnum] = coff_to_elfsym[csi]
                sec_signame[sym.secnum] = sym.name

    def _root(sn, seen):
        sel, num = sec_selection.get(sn, (0, 0))
        if (sel == IMAGE_COMDAT_SELECT_ASSOCIATIVE and num
                and num != sn and num not in seen and num in sec_selection):
            seen.add(sn)
            return _root(num, seen)
        return sn

    groups = {}                        # root elf_shndx -> {"sig":symidx,"members":[elf_shndx]}
    for sn in sec_selection:
        root = _root(sn, set())
        sig = sec_sig.get(root)
        if sig is None:                # no external signature -> left weak above
            continue
        r_elf = coff_to_elfshndx[root]
        g = groups.setdefault(r_elf, {"sig": sig, "members": [],
                                      "signame": sec_signame.get(root, "")})
        g["members"].append(coff_to_elfshndx[sn])

    # COMDAT binding: one STRONG symbol per group -- its signature -- so lld
    # dedups groups by that name AND keeps the (referenced) group section through
    # --gc-sections; a weak signature would let GC drop a section that is only
    # referenced from another object (the vtable deleting-dtor thunks). Every
    # OTHER COMDAT global becomes weak, so two objects that legally define the
    # same COMDAT symbol under different group signatures do not collide as
    # strong duplicates -- COFF COMDAT "pick any", but GC-safe.
    # Exception: if a signature's name is ALSO defined strong out-of-line (in a
    # non-COMDAT section of some object in the archive -- an inline helper one TU
    # emits for real), the COMDAT copies must yield to that strong definition, so
    # its signatures are weak too (noncomdat_strong is passed in by archive mode).
    # Keep a signature STRONG only for CODE groups. The GC hazard the strong
    # signature guards against -- a group referenced only from another object
    # being dropped -- is the vtable deleting-dtor thunk case, which is code.
    # A DATA selectany global (e.g. d3d9.h's `extern const __declspec(selectany)
    # D3DPRIMITIVEVERTEXCOUNT[][2]`) is also emitted by any title TU that
    # includes the header, as clang linkonce_odr (weak); a STRONG signature here
    # loses to nothing yet leaves lld resolving the discarded library copy
    # ("relocation refers to a symbol in a discarded section"). Emit data
    # signatures WEAK so they dedup consistently with the title's weak copy.
    elf_flags = {coff_to_elfshndx[ci]: s.flags for ci, _, s in kept}
    signatures = {g["sig"] for r_elf, g in groups.items()
                  if g["signame"] not in noncomdat_strong
                  and (elf_flags.get(r_elf, 0) & IMAGE_SCN_MEM_EXECUTE)
                  and _keep_code_signature_strong(g["signame"], external_strong)}
    comdat_kept = {coff_to_elfshndx[ci] for ci, _, s in kept
                   if s.flags & IMAGE_SCN_LNK_COMDAT}
    for k, (noff, val, sz, info, other, shndx) in enumerate(elf_syms):
        if ((info >> 4) == STB_GLOBAL and shndx in comdat_kept
                and k not in signatures):
            elf_syms[k] = (noff, val, sz, (STB_WEAK << 4) | (info & 0xF), other, shndx)

    return _write_elf(kept, elf_syms, strtab, first_global, rela,
                      coff_to_elfshndx, groups, elf64=elf64)


def _enumerate_coff_syms(obj):
    """Yield (index, Symbol) skipping the aux records that follow each symbol."""
    i = 0
    for sym in obj.symbols:
        yield i, sym
        i += 1 + sym.naux


def defined_globals(obj):
    """Names of the global symbols this object defines (for the archive index)."""
    kept = {ci for ci, s in enumerate(obj.sections, start=1)
            if _keep_section(s.name, s.flags) is not None}
    out = []
    for _, sym in _enumerate_coff_syms(obj):
        if sym.cls == IMAGE_SYM_CLASS_EXTERNAL and sym.secnum in kept:
            out.append(sym.name)
        # a COMMON symbol (secnum 0, value = size) is a tentative definition
        # that satisfies a reference, so it belongs in the archive index too.
        elif sym.cls == IMAGE_SYM_CLASS_EXTERNAL and sym.secnum == 0 and sym.value > 0:
            out.append(sym.name)
    return out


# ---- System V archive (.a) --------------------------------------------------

def _ar_header(name, size):
    return ("%-16s%-12d%-6d%-6d%-8s%-10d`\n" %
            (name, 0, 0, 0, "0", size)).encode("ascii")


def write_archive(members, path):
    """Write a System V `.a` with a GNU symbol index and longnames member.

    `members` is a list of (name, elf_bytes, [defined_global_symbols]). The
    symbol index (member "/") lets lld pull the right object for each symbol;
    long member names go in the "//" member.
    """
    # long member names -> "//" table; short names stored inline as "name/"
    longnames = bytearray()
    stored_names = []
    for name, _, _ in members:
        short = name + "/"
        if len(short) <= 16:
            stored_names.append(short)
        else:
            stored_names.append("/%d" % len(longnames))
            longnames += name.encode("ascii") + b"/\n"

    # symbol index: 4-byte BE count, count*4-byte BE member offsets, then names.
    symbols = []                        # (symbol_name, member_index)
    for mi, (_, _, defs) in enumerate(members):
        for sym in defs:
            symbols.append((sym, mi))
    index_names = b"".join(s.encode("ascii") + b"\0" for s, _ in symbols)
    index_size = 4 + 4 * len(symbols) + len(index_names)

    magic = len(ARCHIVE_MAGIC)
    # offsets are to each member's header; compute after the two special members
    def padded(n):
        return n + (n & 1)

    off = magic
    off += 60 + padded(index_size)                       # "/" symbol table member
    longnames_size = len(longnames)
    if longnames_size:
        off += 60 + padded(longnames_size)               # "//" longnames member

    member_offsets = []
    cur = off
    for (name, data, _), stored in zip(members, stored_names):
        member_offsets.append(cur)
        cur += 60 + padded(len(data))

    out = bytearray(ARCHIVE_MAGIC)
    # symbol table member
    out += _ar_header("/", index_size)
    out += struct.pack(">I", len(symbols))
    for _, mi in symbols:
        out += struct.pack(">I", member_offsets[mi])
    out += index_names
    if index_size & 1:
        out += b"\n"
    # longnames member
    if longnames_size:
        out += _ar_header("//", longnames_size)
        out += longnames
        if longnames_size & 1:
            out += b"\n"
    # object members
    for (name, data, _), stored in zip(members, stored_names):
        out += _ar_header(stored, len(data))
        out += data
        if len(data) & 1:
            out += b"\n"

    with open(path, "wb") as f:
        f.write(out)
    return len(symbols)


class ElfSec:
    __slots__ = ("name", "typ", "flags", "data", "size", "link", "info",
                 "align", "entsize", "offset", "_nameoff")

    def __init__(self, name, typ, flags=0, data=b"", size=None, link=0,
                 info=0, align=1, entsize=0):
        self.name = name
        self.typ = typ
        self.flags = flags
        self.data = data                        # bytes for everything but NOBITS
        self.size = len(data) if size is None else size
        self.link = link
        self.info = info
        self.align = align
        self.entsize = entsize
        self.offset = 0


def _write_elf(kept, elf_syms, strtab, first_global, rela, coff_to_elfshndx,
               groups=None, elf64=False):
    groups = groups or {}
    # which kept sections belong to a group (get SHF_GROUP): map their kept
    # elf_idx to the group root so their .rela joins the same group too.
    member_root = {}                             # kept elf_idx -> root elf_idx
    for root, g in groups.items():
        for m in g["members"]:
            member_root[m] = root

    # section order: [0] null, kept sections, .symtab, .strtab, one .rela.* per
    # kept section with relocations, then .group sections, then .shstrtab last.
    secs = [ElfSec("", 0)]                       # null section

    kept_elf_index = {}                          # kept elf_idx (1-based) -> secs[] index
    for elf_idx, (ci, name, s) in enumerate(kept, start=1):
        flags = SHF_ALLOC
        if s.flags & IMAGE_SCN_MEM_WRITE:
            flags |= SHF_WRITE
        if s.flags & IMAGE_SCN_MEM_EXECUTE:
            flags |= SHF_EXECINSTR
        if elf_idx in member_root:
            flags |= SHF_GROUP
        nalign = (s.flags >> 20) & 0xF           # COFF align is (n+1) in this nibble
        align = (1 << (nalign - 1)) if nalign else 4
        kept_elf_index[elf_idx] = len(secs)
        if s.flags & IMAGE_SCN_CNT_UNINITIALIZED_DATA:
            secs.append(ElfSec(name, SHT_NOBITS, flags, b"",
                               size=s.vsize or s.rawsize, align=align))
        else:
            secs.append(ElfSec(name, SHT_PROGBITS, flags, s.data, align=align))

    # elf_syms tuples are (name, value, size, info, other, shndx). Elf32_Sym is
    # name,value,size,info,other,shndx; Elf64_Sym reorders to
    # name,info,other,shndx,value,size with 8-byte value/size.
    if elf64:
        sym_bytes = b"".join(struct.pack(">IBBHQQ", n, inf, oth, shn, val, sz)
                             for (n, val, sz, inf, oth, shn) in elf_syms)
        sym_entsize, sym_align = 24, 8
    else:
        sym_bytes = b"".join(struct.pack(">IIIBBH", *s) for s in elf_syms)
        sym_entsize, sym_align = 16, 4
    symtab_idx = len(secs)
    secs.append(ElfSec(".symtab", SHT_SYMTAB, data=sym_bytes, link=symtab_idx + 1,
                       info=first_global, align=sym_align, entsize=sym_entsize))
    secs.append(ElfSec(".strtab", SHT_STRTAB, data=bytes(strtab.buf), align=1))

    # group root elf_idx -> list of secs[] indices that are its members (the
    # COMDAT section(s) plus, appended just below, their .rela sections).
    group_secidx = {root: [] for root in groups}
    for elf_idx in member_root:
        group_secidx[member_root[elf_idx]].append(kept_elf_index[elf_idx])

    for elf_idx, entries in rela.items():
        target = kept_elf_index[elf_idx]
        rflags = SHF_GROUP if elf_idx in member_root else 0
        # Elf32_Rela: offset(4), info=sym<<8|type (4), addend(4).
        # Elf64_Rela: offset(8), info=sym<<32|type (8), addend(8).
        if elf64:
            blob = b"".join(struct.pack(">QQq", off, (sym << 32) | rtype, add)
                            for off, sym, rtype, add in entries)
            rela_entsize, rela_align = 24, 8
        else:
            blob = b"".join(struct.pack(">IIi", off, (sym << 8) | rtype, add)
                            for off, sym, rtype, add in entries)
            rela_entsize, rela_align = 12, 4
        secs.append(ElfSec(".rela" + secs[target].name, SHT_RELA, data=blob,
                           link=symtab_idx, info=target, align=rela_align,
                           entsize=rela_entsize, flags=rflags))
        if elf_idx in member_root:
            group_secidx[member_root[elf_idx]].append(len(secs) - 1)

    # one SHT_GROUP section per COMDAT group; sh_info = signature symbol index,
    # data = GRP_COMDAT flag followed by each member section's header index.
    for root, g in groups.items():
        members = group_secidx[root]
        blob = struct.pack(">I", GRP_COMDAT) + b"".join(
            struct.pack(">I", m) for m in members)
        secs.append(ElfSec(".group", SHT_GROUP, data=blob, link=symtab_idx,
                           info=g["sig"], align=4, entsize=4))

    shstr = StrTab()
    for sec in secs:
        sec._nameoff = shstr.add(sec.name)
    shstrtab_idx = len(secs)
    secs.append(ElfSec(".shstrtab", SHT_STRTAB, align=1))
    secs[-1]._nameoff = shstr.add(".shstrtab")
    secs[-1].data = bytes(shstr.buf)
    secs[-1].size = len(secs[-1].data)

    # lay out: ELF header (52 for ELF32, 64 for ELF64), then each non-empty
    # section's bytes (aligned), then the section header table.
    ehsize = 64 if elf64 else 52
    shentsize = 64 if elf64 else 40
    align_end = 8 if elf64 else 4
    offset = ehsize
    for sec in secs:
        if sec.typ in (0, SHT_NOBITS):
            sec.offset = 0
            continue
        if sec.align > 1 and offset % sec.align:
            offset += sec.align - (offset % sec.align)
        sec.offset = offset
        offset += len(sec.data)

    if offset % align_end:
        offset += align_end - (offset % align_end)
    shoff = offset

    out = bytearray()
    ei_class = 2 if elf64 else 1
    e_ident = b"\x7fELF" + bytes([ei_class, 2, 1, 0]) + b"\0" * 8   # BE, ver1
    out += e_ident
    if elf64:
        # Elf64_Ehdr: type,machine,version, entry(8),phoff(8),shoff(8), flags,
        # ehsize,phentsize,phnum,shentsize,shnum,shstrndx.
        out += struct.pack(">HHIQQQIHHHHHH",
                           ET_REL, EM_PPC64, 1, 0, 0, shoff, 0,
                           ehsize, 0, 0, shentsize, len(secs), shstrtab_idx)
    else:
        out += struct.pack(">HHIIIIIHHHHHH",
                           ET_REL, EM_PPC, 1, 0, 0, shoff, 0,
                           ehsize, 0, 0, shentsize, len(secs), shstrtab_idx)
    out += b"\0" * (ehsize - len(out))

    for sec in secs:
        if sec.typ in (0, SHT_NOBITS) or not sec.data:
            continue
        if len(out) < sec.offset:
            out += b"\0" * (sec.offset - len(out))
        out += sec.data
    if len(out) < shoff:
        out += b"\0" * (shoff - len(out))

    for sec in secs:
        if elf64:
            # Elf64_Shdr: name,type, flags(8),addr(8),offset(8),size(8),
            # link,info, addralign(8),entsize(8).
            out += struct.pack(">IIQQQQIIQQ",
                               sec._nameoff, sec.typ, sec.flags, 0, sec.offset,
                               sec.size, sec.link, sec.info, sec.align,
                               sec.entsize)
        else:
            out += struct.pack(">IIIIIIIIII",
                               sec._nameoff, sec.typ, sec.flags, 0, sec.offset,
                               sec.size, sec.link, sec.info, sec.align,
                               sec.entsize)
    return bytes(out)


# ---- reporting --------------------------------------------------------------

def first_coff(path):
    blob = open(path, "rb").read()
    for member, longnames in read_archive(blob):
        if member.name in ("/", "//"):
            continue
        if member.data[:2] == struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
            return member_name(member.name, longnames), CoffObject(member.data)
    sys.exit("no PowerPC COFF member found")


def cmd_dump(path):
    name, obj = first_coff(path)
    print(f"{path} :: {name}")
    print(f"  machine 0x{obj.machine:04X}  sections {obj.nsections}  "
          f"symbols {obj.nsymbols}  chars 0x{obj.characteristics:04X}")
    print("  sections:")
    for s in obj.sections:
        rt = Counter(REL_TYPES.get(t, f"0x{t:X}") for _, _, t in s.relocs)
        rt_str = " ".join(f"{k}={v}" for k, v in sorted(rt.items()))
        print(f"    {s.name:<10} size {s.rawsize:>7}  flags 0x{s.flags:08X}  "
              f"relocs {s.nreloc:>4}  {rt_str}")
    print("  first 12 symbols:")
    shown = 0
    for sym in obj.symbols:
        if shown >= 12:
            break
        print(f"    [{sym.secnum:>3}] cls {sym.cls:>3} val 0x{sym.value:08X}  {sym.name}")
        shown += 1


def cmd_survey(paths):
    reltypes = Counter()
    sections = Counter()
    classes = Counter()
    machines = Counter()
    nobjs = 0
    for path in paths:
        blob = open(path, "rb").read()
        for member, longnames in read_archive(blob):
            if member.name in ("/", "//"):
                continue
            if member.data[:2] != struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
                machines[member.data[:2].hex()] += 1
                continue
            obj = CoffObject(member.data)
            machines[f"{obj.machine:04X}"] += 1
            nobjs += 1
            for s in obj.sections:
                sections[s.name.rstrip("0123456789") or s.name] += 1
                for _, _, t in s.relocs:
                    reltypes[REL_TYPES.get(t, f"UNKNOWN_0x{t:X}")] += 1
            for sym in obj.symbols:
                classes[STORAGE_CLASS.get(sym.cls, f"cls{sym.cls}")] += 1
    print(f"objects: {nobjs}")
    print(f"machines: {dict(machines)}")
    print("relocation types:")
    for k, v in reltypes.most_common():
        print(f"  {k:<16} {v}")
    print("section names (numeric suffix stripped):")
    for k, v in sections.most_common(20):
        print(f"  {k:<16} {v}")
    print("symbol storage classes:")
    for k, v in classes.most_common():
        print(f"  {k:<16} {v}")


def find_member(path, needle):
    """Return (name, CoffObject) for the first PPC member whose name contains needle."""
    blob = open(path, "rb").read()
    for member, longnames in read_archive(blob):
        if member.name in ("/", "//"):
            continue
        if member.data[:2] != struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
            continue
        name = member_name(member.name, longnames)
        if needle in name:
            return name, CoffObject(member.data)
    sys.exit(f"no PowerPC member matching {needle!r}")


def cmd_emit(path, member, out, elf64=False):
    name, obj = find_member(path, member)
    print(f"translating {name}")
    elf = coff_to_elf(obj, elf64=elf64)
    with open(out, "wb") as f:
        f.write(elf)
    print(f"wrote {out} ({len(elf)} bytes)")


def cmd_translate_all(path):
    """Translate every PPC object in the archive, reporting what does not fit.

    Scaling from one object to all of them is where the assumptions that held
    for a single clean object break, so this drives the emitter over the whole
    library and tallies warnings and failures rather than writing anything.
    """
    blob = open(path, "rb").read()
    n_ok = n_fail = n_import = 0
    warnings = Counter()
    failures = []
    for member, longnames in read_archive(blob):
        if member.name in ("/", "//"):
            continue
        if member.data[:2] != struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
            n_import += 1
            continue
        name = member_name(member.name, longnames)
        msgs = []
        try:
            obj = CoffObject(member.data)
            coff_to_elf(obj, warn=lambda m: msgs.append(m.strip()))
            n_ok += 1
            for m in msgs:
                warnings[m.split(" at ")[0].split(" 0x")[0].strip()] += 1
        except Exception as e:                                # noqa: surface, don't hide
            n_fail += 1
            if len(failures) < 20:
                failures.append(f"{name}: {type(e).__name__}: {e}")
    print(f"{path}")
    print(f"  translated {n_ok}, failed {n_fail}, import stubs {n_import}")
    if warnings:
        print("  warnings:")
        for k, v in warnings.most_common():
            print(f"    {v:>6}  {k}")
    if failures:
        print("  failures (first 20):")
        for f in failures:
            print(f"    {f}")


def parse_short_import(data):
    """Return (symbol, dll_ordinal_or_hint, name_type, import_type) for a
    short-import member. import_type is IMPORT_OBJECT_CODE=0, _DATA=1, _CONST=2
    (winnt.h) -- DATA/CONST mark a data export (a variable), not a callable."""
    # IMPORT_OBJECT_HEADER: Sig1(2) Sig2(2) Version(2) Machine(2) TimeDate(4)
    # SizeOfData(4) OrdinalOrHint(2) TypeBits(2); then symbol\0 dll\0
    # TypeBits: bits 0-1 = ImportType, bits 2-4 = NameType.
    ordhint, typebits = struct.unpack_from("<HH", data, 16)
    import_type = typebits & 0x3
    name_type = (typebits >> 2) & 0x7
    strings = data[20:]
    sym = strings.split(b"\0")[0].decode("ascii", "replace")
    rest = strings[len(sym) + 1:]
    dll = rest.split(b"\0")[0].decode("ascii", "replace")
    return sym, dll, ordhint, name_type, import_type


def runtime_defined_symbols(path):
    """Defined external symbol names of a (GNU/llvm-ar) archive, read straight
    from its armap symbol-index member -- no need to parse the ELF objects. Used
    to build the external-strong set: names the runtime (libc.a/libcpp.a) owns
    strong, so an XDK archive's COMDAT copy of the same name must stay strong
    (see _keep_code_signature_strong)."""
    try:
        blob = open(path, "rb").read()
    except OSError:
        return set()
    if blob[:8] != ARCHIVE_MAGIC:
        return set()
    pos = 8
    while pos + 60 <= len(blob):
        header = blob[pos:pos + 60]
        name = header[0:16].decode("ascii", "replace").rstrip()
        size = int(header[48:58].decode("ascii").strip())
        data = blob[pos + 60:pos + 60 + size]
        pos = pos + 60 + size + (size & 1)
        if name == "/":                         # GNU armap: BE count, offsets, names
            if len(data) < 4:
                return set()
            n = struct.unpack(">I", data[:4])[0]
            names = data[4 + 4 * n:]
            return {s.decode("latin1") for s in names.split(b"\0") if s}
        if name and name != "//":               # first real member, no armap
            break
    return set()


def cmd_archive(path, out_a, out_manifest, runtime_libs=(), elf64=False):
    import json
    blob = open(path, "rb").read()
    members = []
    imports = []
    seen = {}                           # unique member names
    # First pass: every COFF object, plus the union of names each defines strong
    # out-of-line (non-COMDAT) -- COMDAT copies of those must yield to them.
    external_strong = set()             # names the runtime archives own strong
    for rl in runtime_libs:
        external_strong |= runtime_defined_symbols(rl)
    coff_members = []                   # (member, longnames, obj)
    noncomdat_strong = set()
    for member, longnames in read_archive(blob):
        if member.name in ("/", "//"):
            continue
        if member.data[:2] != struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
            sym, dll, ordhint, nt, it = parse_short_import(member.data)
            imports.append({"symbol": sym, "dll": dll, "ordinal_or_hint": ordhint,
                            "name_type": nt, "import_type": it})
            continue
        obj = CoffObject(member.data)
        noncomdat_strong |= strong_noncomdat_globals(obj)
        coff_members.append((member, longnames, obj))

    # Second pass: translate each object, weakening COMDAT signatures that clash
    # with an out-of-line strong definition seen anywhere in the archive.
    for member, longnames, obj in coff_members:
        elf = coff_to_elf(obj, warn=lambda m: None,
                          noncomdat_strong=noncomdat_strong,
                          external_strong=external_strong, elf64=elf64)
        base = member_name(member.name, longnames).replace("\\", "/").split("/")[-1]
        base = base[:-4] if base.endswith(".obj") else base
        n = seen.get(base, 0)
        seen[base] = n + 1
        uniq = f"{base}.{n}.o" if n else f"{base}.o"
        members.append((uniq, elf, defined_globals(obj)))

    nsyms = write_archive(members, out_a)
    with open(out_manifest, "w") as f:
        json.dump({"library": path, "objects": len(members),
                   "index_symbols": nsyms, "imports": imports}, f, indent=2)
    print(f"{out_a}: {len(members)} objects, {nsyms} indexed symbols")
    print(f"{out_manifest}: {len(imports)} import stubs")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump")
    d.add_argument("lib")
    s = sub.add_parser("survey")
    s.add_argument("libs", nargs="+")
    e = sub.add_parser("emit", help="translate one object to a PPC ELF")
    e.add_argument("lib")
    e.add_argument("member", help="substring of the member (object) name")
    e.add_argument("-o", "--out", default="out.o")
    e.add_argument("--ppc64", action="store_true",
                   help="emit ELFCLASS64/EM_PPC64 (powerpc64-unknown-xbox360) "
                        "instead of ELF32; 32-bit content unchanged")
    t = sub.add_parser("translate-all", help="translate every object, report problems")
    t.add_argument("lib")
    a = sub.add_parser("archive", help="translate a whole .lib to an ELF .a + import manifest")
    a.add_argument("lib")
    a.add_argument("-o", "--out", default="out.a")
    a.add_argument("-m", "--manifest", default=None)
    a.add_argument("--runtime", action="append", default=[],
                   help="a runtime archive (libc.a/libcpp.a) whose defined symbols "
                        "form the external-strong set; repeatable")
    a.add_argument("--ppc64", action="store_true",
                   help="emit ELFCLASS64/EM_PPC64 (powerpc64-unknown-xbox360) "
                        "instead of ELF32; 32-bit content unchanged")
    args = ap.parse_args()

    if args.cmd == "dump":
        cmd_dump(args.lib)
    elif args.cmd == "survey":
        cmd_survey(args.libs)
    elif args.cmd == "emit":
        cmd_emit(args.lib, args.member, args.out, elf64=args.ppc64)
    elif args.cmd == "translate-all":
        cmd_translate_all(args.lib)
    elif args.cmd == "archive":
        manifest = args.manifest or (args.out.rsplit(".", 1)[0] + ".imports.json")
        cmd_archive(args.lib, args.out, manifest, runtime_libs=args.runtime,
                    elf64=args.ppc64)


if __name__ == "__main__":
    main()
