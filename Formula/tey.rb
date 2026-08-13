class Tey < Formula
  desc "Package and Kex toolchain manager"
  homepage "https://github.com/kexhq/kex"
  license "MIT"
  head "https://github.com/kexhq/kex.git", branch: "main"

  depends_on "cmake"
  depends_on "boost"
  depends_on "erlang"
  depends_on "gmp"
  depends_on "pcre2"
  depends_on "readline"
  depends_on "openssl@3" unless OS.mac?

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
                    "-DCMAKE_BUILD_TYPE=Release"
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
    system "make", "-C", "tey", "install", "PREFIX=#{prefix}",
                   "KEX=#{buildpath}/build/kex", "INSTALL_NONINTERACTIVE=1"
  end

  def caveats
    <<~EOS
      Tey and a matching Kex compiler/runtime/stdlib toolchain were installed.
      To enable automatic tey.lock conflict regeneration, run:
        tey setup merge-driver

      Until the first tagged release, install this formula with --HEAD.
    EOS
  end

  test do
    assert_match "tey 0.1.0", shell_output("#{bin}/tey --version")
    assert_match "kex 0.3.0", shell_output("#{bin}/kex --version")
  end
end
