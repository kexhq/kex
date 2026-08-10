#!/usr/bin/env python3
"""Regenerate src/common/unicode_case.hxx — Unicode case-mapping tables.

Kex needs BOTH mappings, because the two operations have different types:

  Char.upperCase   : Char -> Char     needs the SIMPLE (1:1) mapping
  String.upperCase : String -> String needs the FULL mapping, which may expand
                                      ("straße" -> "STRASSE")

Sources, both already on the machine — no network fetch, no vendored data file:

  * FULL mappings come from Python's own Unicode database, via per-codepoint
    str.upper()/str.lower(). Applying it one codepoint at a time deliberately
    skips Python's context-sensitive rules (final sigma), which is what BEAM's
    string:uppercase/1 does too.
  * SIMPLE mappings are str.upper()/str.lower() wherever those stay a single
    codepoint. For the ~100 codepoints that expand, the simple mapping is NOT
    derivable from the full one — U+00DF has none (ß stays ß) while U+1FB3 has
    U+1FBC — so those come from Perl's Unicode::UCD, which exposes the real
    UnicodeData.txt fields.

This was cross-checked against `string:uppercase/1` and `string:lowercase/1`
over all 1.1M codepoints on OTP: zero disagreements on any mapping both know.
Python's database is simply newer, so it carries some mappings an older OTP
lacks; those codepoints case-map on the interpreter and not on BEAM until OTP
catches up.

The BEAM side needs a sliver of this too. OTP has no simple-mapping function
at all: string:to_upper/1 is the pre-Unicode one (ISO-8859-1 only) and
string:uppercase/1 is the full mapping. "Full, unless it expands" recovers the
simple mapping for every codepoint but the ~55 where a real simple mapping
exists alongside an expanding full one (the Greek ypogegrammeni block, and
U+0130). Those get emitted as Erlang clauses so both backends agree.

Usage:
  python3 tools/unicode_case_gen.py > src/common/unicode_case.hxx
  python3 tools/unicode_case_gen.py --erlang > runtime/src/kex_unicode_case.erl
"""
import subprocess
import sys

MAX = 0x110000


def surrogate(cp):
    return 0xD800 <= cp <= 0xDFFF


def full_maps():
    upper, lower = {}, {}
    for cp in range(MAX):
        if surrogate(cp):
            continue
        c = chr(cp)
        u, l = c.upper(), c.lower()
        if u != c:
            upper[cp] = [ord(x) for x in u]
        if l != c:
            lower[cp] = [ord(x) for x in l]
    return upper, lower


def perl_simple(codepoints):
    """The real UnicodeData.txt simple mappings, for the given codepoints."""
    script = r"""
use Unicode::UCD qw(charinfo);
while (my $line = <STDIN>) {
    chomp $line;
    my $info = charinfo(hex $line);
    my $u = ($info && $info->{upper}) ? $info->{upper} : "";
    my $l = ($info && $info->{lower}) ? $info->{lower} : "";
    print "$line|$u|$l\n";
}
"""
    query = "\n".join("%X" % cp for cp in codepoints)
    out = subprocess.run(["perl", "-e", script], input=query, text=True,
                         capture_output=True, check=True).stdout
    upper, lower = {}, {}
    for line in out.splitlines():
        # Pipe-delimited, NOT whitespace: an absent mapping is an empty field,
        # and whitespace splitting would silently shift the lower mapping into
        # the upper slot (U+0130 has no simple uppercase but does have a lower).
        code, up, low = line.split("|")
        cp = int(code, 16)
        if up:
            upper[cp] = int(up, 16)
        if low:
            lower[cp] = int(low, 16)
    return upper, lower


def simple_maps(full_upper, full_lower):
    expanding = sorted(set(cp for cp, m in full_upper.items() if len(m) > 1) |
                       set(cp for cp, m in full_lower.items() if len(m) > 1))
    ucd_upper, ucd_lower = perl_simple(expanding)
    upper, lower = {}, {}
    for cp, m in full_upper.items():
        target = m[0] if len(m) == 1 else ucd_upper.get(cp)
        if target is not None and target != cp:
            upper[cp] = target
    for cp, m in full_lower.items():
        target = m[0] if len(m) == 1 else ucd_lower.get(cp)
        if target is not None and target != cp:
            lower[cp] = target
    return upper, lower


def runs(mapping):
    """Compress to (start, end, stride, delta).

    Case blocks are overwhelmingly either contiguous with a constant delta
    (ASCII, Cyrillic) or alternating upper/lower pairs (Latin Extended-A), so
    a stride of 1 or 2 collapses the table by roughly an order of magnitude.
    """
    items = sorted(mapping.items())
    out = []
    i = 0
    while i < len(items):
        cp, target = items[i]
        delta = target - cp
        best = (cp, cp, 1, delta)
        for stride in (1, 2):
            end, j = cp, i + 1
            while (j < len(items) and items[j][0] == end + stride
                   and items[j][1] - items[j][0] == delta):
                end = items[j][0]
                j += 1
            if end > best[1]:
                best = (cp, end, stride, delta)
        out.append(best)
        start, end, stride, _ = best
        consumed = (end - start) // stride + 1
        i += consumed
    return out


def emit_runs(name, table):
    print("inline constexpr CaseRun %s[] = {" % name)
    for start, end, stride, delta in table:
        print("    {0x%04X, 0x%04X, %d, %d}," % (start, end, stride, delta))
    print("};")
    print()


def emit_full(name, mapping):
    print("inline constexpr FullCase %s[] = {" % name)
    for cp, m in sorted(mapping.items()):
        padded = m + [0] * (3 - len(m))
        print("    {0x%04X, %d, {0x%04X, 0x%04X, 0x%04X}}," %
              (cp, len(m), *padded))
    print("};")
    print()


def emit_erlang(simple_upper, simple_lower, expand_upper, expand_lower):
    """The codepoints where "full unless it expands" misses the simple mapping."""
    up = sorted((cp, simple_upper[cp]) for cp in expand_upper
                if cp in simple_upper)
    low = sorted((cp, simple_lower[cp]) for cp in expand_lower
                 if cp in simple_lower)
    print("%% GENERATED by tools/unicode_case_gen.py — do not edit by hand.")
    print("%%")
    print("%% Unicode simple (1:1) case mapping, for Char -> Char. OTP exposes")
    print("%% no such function: string:to_upper/1 is the pre-Unicode one and")
    print("%% only covers ISO-8859-1, while string:uppercase/1 is the FULL")
    print("%% mapping and may expand (ß -> SS). Taking the full mapping only")
    print("%% when it stays one codepoint recovers the simple mapping almost")
    print("%% everywhere; the clauses below are the codepoints where that is")
    print("%% wrong, because they have BOTH an expanding full mapping and a")
    print("%% real simple one. Kept in step with src/common/unicode_case.hxx,")
    print("%% which is what the interpreter uses.")
    print("-module(kex_unicode_case).")
    print("-export([simple_upper/1, simple_lower/1]).")
    print()
    print("simple_upper(C) ->")
    print("    case string:uppercase([C]) of")
    print("        [Mapped] -> Mapped;")
    print("        _ -> upper_exception(C)")
    print("    end.")
    print()
    print("simple_lower(C) ->")
    print("    case string:lowercase([C]) of")
    print("        [Mapped] -> Mapped;")
    print("        _ -> lower_exception(C)")
    print("    end.")
    print()
    for name, pairs in (("upper_exception", up), ("lower_exception", low)):
        for cp, target in pairs:
            print("%s(16#%04X) -> 16#%04X;" % (name, cp, target))
        print("%s(C) -> C." % name)
        print()


def main():
    full_upper, full_lower = full_maps()
    simple_upper, simple_lower = simple_maps(full_upper, full_lower)
    expand_upper = {cp: m for cp, m in full_upper.items() if len(m) > 1}
    expand_lower = {cp: m for cp, m in full_lower.items() if len(m) > 1}

    if "--erlang" in sys.argv:
        emit_erlang(simple_upper, simple_lower, expand_upper, expand_lower)
        return

    print("#pragma once")
    print("// GENERATED by tools/unicode_case_gen.py — do not edit by hand.")
    print("//")
    print("// Unicode %s case mappings. `simple*` is the 1:1 mapping a Char"
          % __import__("unicodedata").unidata_version)
    print("// uses; `expand*` holds only the codepoints whose FULL mapping is")
    print("// longer than one codepoint (ß -> SS), which a String uses on top of")
    print("// the simple table. See the generator for how the two were sourced.")
    print("#include <cstdint>")
    print()
    print("namespace kex::utf8::tables {")
    print()
    print("// [start, end] stepping by `stride`, each mapping to itself + delta.")
    print("struct CaseRun {")
    print("    char32_t start;")
    print("    char32_t end;")
    print("    std::uint8_t stride;")
    print("    std::int32_t delta;")
    print("};")
    print()
    print("// A full mapping that is longer than the source codepoint.")
    print("struct FullCase {")
    print("    char32_t from;")
    print("    std::uint8_t length;")
    print("    char32_t to[3];")
    print("};")
    print()
    emit_runs("simpleUpper", runs(simple_upper))
    emit_runs("simpleLower", runs(simple_lower))
    emit_full("expandUpper", expand_upper)
    emit_full("expandLower", expand_lower)
    print("} // namespace kex::utf8::tables")

    print("simple upper %d entries -> %d runs" %
          (len(simple_upper), len(runs(simple_upper))), file=sys.stderr)
    print("simple lower %d entries -> %d runs" %
          (len(simple_lower), len(runs(simple_lower))), file=sys.stderr)
    print("expanding upper %d, lower %d" %
          (len(expand_upper), len(expand_lower)), file=sys.stderr)


if __name__ == "__main__":
    main()
