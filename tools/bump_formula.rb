# Points kexhq/homebrew-tey's formula at a published Kex release: rewrites the
# two STABLE marker blocks in Formula/tey.rb with the release's urls and the
# checksums published beside it.
#
# Called by the `formula` job in .github/workflows/release.yml, and exercised on
# every pull request by CI's `formula-bump` job against the latest release. It
# lives in a file rather than a heredoc inside the workflow for exactly that
# reason: an embedded script cannot be syntax-checked or run outside a release,
# and the one that used to be here shipped an indentation error that sat latent
# until the first stable release — the `formula` job is gated on
# `prerelease == 'false'`, so nothing had ever run it.
#
#   ruby tools/bump_formula.rb <base-url> <version> [formula-path] [hash-dir]
#
# <hash-dir> holds one `<archive-name>.hash` per archive, each a bare sha256.
# The checksums are the ones published beside the release rather than any
# computed here: if an asset and its .sha256 ever disagreed, this would be the
# wrong place to find out.

USAGE = "usage: bump_formula.rb <base-url> <version> [formula-path] [hash-dir]"

base, version, formula_path, hash_dir = ARGV
abort(USAGE) if base.nil? || base.empty? || version.nil? || version.empty?
formula_path ||= "Formula/tey.rb"
hash_dir ||= "."

def digest(hash_dir, name)
  path = File.join(hash_dir, "#{name}.hash")
  abort("missing checksum #{path}") unless File.file?(path)
  value = File.read(path).strip
  # A truncated download that still exits 0 would otherwise reach the formula
  # as a plausible-looking `sha256 ""`, which brew only rejects at install.
  abort("#{path} is not a sha256: #{value.inspect}") unless value.match?(/\A[0-9a-f]{64}\z/)
  value
end

# Anchored on the marker pair so a formula that grew or lost a block fails here
# rather than silently emitting one that installs nothing.
def fill(formula, marker, body, formula_path)
  pattern = /(  \# <<#{marker}\n).*?(  \# #{marker}>>\n)/m
  found = formula.scan(pattern).size
  abort("expected one #{marker} block in #{formula_path}, found #{found}") unless found == 1
  formula.sub(pattern) { "#{Regexp.last_match(1)}#{body}#{Regexp.last_match(2)}" }
end

# `on_macos`/`on_arm` rather than one url with a substitution: brew resolves
# these at install time on the machine doing the installing, and only downloads
# the one it picks. macOS is arm-only for now — Intel macOS publishes no archive
# to point a resource at.
def resource(base, version, hash_dir, target, indent)
  name = "kex-#{version}-#{target}"
  pad = " " * indent
  %(#{pad}resource "kex" do\n) +
    %(#{pad}  url "#{base}/#{name}.tar.gz"\n) +
    %(#{pad}  sha256 "#{digest(hash_dir, name)}"\n) +
    %(#{pad}end\n)
end

formula = File.read(formula_path)

# Tey itself is compiled to BEAM modules and therefore architecture-independent:
# one small archive serves every platform.
tey = "tey-#{version}"
formula = fill(formula, "STABLE-TEY",
               %(  url "#{base}/#{tey}.tar.gz"\n  sha256 "#{digest(hash_dir, tey)}"\n),
               formula_path)

kex = "  on_macos do\n" + resource(base, version, hash_dir, "macos-arm64", 4) + "  end\n\n"
kex += "  on_linux do\n"
kex += "    on_arm do\n" + resource(base, version, hash_dir, "linux-arm64", 6) + "    end\n"
kex += "    on_intel do\n" + resource(base, version, hash_dir, "linux-x86_64", 6) + "    end\n"
kex += "  end\n"
formula = fill(formula, "STABLE-KEX", kex, formula_path)

File.write(formula_path, formula)
