#!/usr/bin/env ruby
# frozen_string_literal: true

# Regression test for kexhq/kex#279: "tey: every interactive prompt answers
# itself -- bin/tey backgrounds erl, so stdin is /dev/null".
#
# `tey/bin/tey` backgrounds `erl` (so a terminal's Ctrl+C can be turned into
# the SIGTERM that runs kex_child_guard) -- and per POSIX, an asynchronous
# list's stdin is /dev/null unless the command redirects it explicitly.
# Every interactive prompt Tey has (askLibrary, tey new's own,
# Tey.Generator's) then reads eof at once and silently takes its default,
# even on a real terminal.
#
# This can ONLY be caught by actually running the launcher SCRIPT attached
# to a real pty -- a plain pipe does not reproduce it (a pipe's other end
# is real, open, data-bearing stdin; the bug is specifically POSIX's
# "backgrounded AND no controlling terminal" substitution), which is also
# why it was invisible in CI before: "not visible in CI, where stdin is a
# pipe or closed and the default answer is right anyway" (#279).
#
# Deliberately does not drive `tey new` itself: that needs a compiled
# tey/ebin, tying this test's ability to catch a stdin regression to
# whether Tey's OWN (much larger, independently changing) source happens
# to compile that day. Instead it compiles a two-line throwaway Kex
# program (PROBE_SOURCE) that reads one line and reports what it got, and
# runs THAT under tey/bin/tey by pointing TEY_EBIN at it -- exercising
# exactly the launcher's stdin plumbing the bug and fix are about, nothing
# else.
#
# Run with: ruby tools/test-tey-pty-prompt.rb [./build/kex] [tey/bin/tey]

require 'pty'
require 'fileutils'
require 'timeout'
require 'tmpdir'

ROOT = File.expand_path('..', __dir__)
KEX = File.expand_path(ARGV.shift || File.join(ROOT, 'build/kex'))
TEY_LAUNCHER = File.expand_path(ARGV.shift || File.join(ROOT, 'tey/bin/tey'))

PROBE_SOURCE = <<~KEX_SOURCE
  main(args) do
    IO.printLine("GOT:${IO.getLine.or("NONE")}")
    System.exit(0)
  end
KEX_SOURCE

abort "#{KEX} not found -- run `make build` first" unless File.exist?(KEX)
abort "#{TEY_LAUNCHER} not found" unless File.exist?(TEY_LAUNCHER)

failed = false

Dir.mktmpdir('kex-tey-pty-test-') do |dir|
  probe_source = File.join(dir, 'main.kex')
  File.write(probe_source, PROBE_SOURCE)
  ebin = File.join(dir, 'ebin')
  FileUtils.mkdir_p(ebin)

  compiled = system(KEX, '--compile', '-o', ebin, probe_source,
                    out: File::NULL, err: File::NULL)
  abort 'failed to compile the throwaway probe program' unless compiled

  output = +''
  # PTY.spawn's block form waits on the child itself once the block
  # returns -- an explicit Process.wait here would race it and raise
  # Errno::ECHILD.
  PTY.spawn({ 'TEY_EBIN' => ebin }, TEY_LAUNCHER) do |readable, writable, pid|
    writable.write("hello-from-pty\n")
    begin
      Timeout.timeout(20) do
        loop { output << readable.readpartial(512) }
      end
    rescue EOFError, Errno::EIO
      # The child closed its end -- normal completion, not a failure.
    rescue Timeout::Error
      warn 'the launcher did not exit within 20s -- killing it'
      begin
        Process.kill('KILL', pid)
      rescue Errno::ESRCH
        nil
      end
      failed = true
    end
  end

  if output.include?('GOT:hello-from-pty')
    puts 'PASS: tey launcher reads a line typed at a real terminal'
  else
    failed = true
    puts "FAIL: expected the line written to the pty to be read back; got #{output.inspect}"
    puts '  If this instead contains GOT:NONE, the launcher raced to end-of-input'
    puts '  without waiting (kexhq/kex#279 is back).'
  end
end

exit(failed ? 1 : 0)
