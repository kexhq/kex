#!/usr/bin/env ruby
# frozen_string_literal: true

# Process.stream runs its child on the caller's own stdin, stdout and
# stderr, on both backends (kexhq/kex#572). That is what lets `tey console`
# give its REPL a terminal: completion, history and line editing. On BEAM it
# used to be a port over a pipe, with a feeder relaying stdin a line at a
# time and stderr folded into stdout -- a child asking whether it was talking
# to a terminal was told no.
#
# spec/process_stream_inherits.kex pins what the runners can see (output
# order, exit codes, a missing program); neither can be checked there:
#
#   * a terminal. The spec runners capture output through pipes and feed no
#     stdin, so only a real pty can show the child that its stdin and stdout
#     are a terminal, and that a line typed there reaches it.
#   * stderr kept apart. The walker suite reads stdout and stderr merged in
#     the order written, the BEAM suite reads them one after the other, so
#     one golden file cannot say where a child's stderr went. Here the two
#     are captured separately and asked directly.
#
# Each check runs on BEAM and on the tree walker.
#
# Run with: ruby tools/test-process-stream-pty.rb [./build/kex]

require 'pty'
require 'open3'
require 'timeout'
require 'tmpdir'

ROOT = File.expand_path('..', __dir__)
KEX = File.expand_path(ARGV.shift || File.join(ROOT, 'build/kex'))

# The child says whether it has a terminal, then reads one line from it.
TERMINAL_SOURCE = <<~'KEX_SOURCE'
  main do
    let script = "if [ -t 0 ] && [ -t 1 ]; then echo CHILD-TTY; else echo CHILD-PIPE; fi; read -r line; echo \"CHILD-GOT:$line\""
    match Process.stream("sh", ["-c", script]) do
      Ok(code) => IO.printLine("EXIT:${code}")
      Error(error) => IO.printLine("ERROR:${error}")
    end
  end
KEX_SOURCE

# The child writes one line to each of its streams.
STREAMS_SOURCE = <<~'KEX_SOURCE'
  main do
    match Process.stream("sh", ["-c", "echo CHILD-OUT; echo CHILD-ERR 1>&2"]) do
      Ok(code) => IO.printLine("EXIT:${code}")
      Error(error) => IO.printLine("ERROR:${error}")
    end
  end
KEX_SOURCE

BACKENDS = { 'BEAM' => [], 'walker' => ['-R'] }.freeze

abort "#{KEX} not found -- run `make build` first" unless File.exist?(KEX)

failed = false

def on_pty(command, line)
  output = +''
  # PTY.spawn's block form waits on the child itself once the block returns.
  PTY.spawn(*command) do |readable, writable, pid|
    writable.write("#{line}\n")
    begin
      Timeout.timeout(60) do
        loop { output << readable.readpartial(512) }
      end
    rescue EOFError, Errno::EIO
      # The child closed its end -- normal completion.
    rescue Timeout::Error
      begin
        Process.kill('KILL', pid)
      rescue Errno::ESRCH
        nil
      end
      output << "\n(timed out after 60s)"
    end
  end
  output
end

Dir.mktmpdir('kex-process-stream-pty-') do |dir|
  terminal = File.join(dir, 'terminal.kex')
  streams = File.join(dir, 'streams.kex')
  File.write(terminal, TERMINAL_SOURCE)
  File.write(streams, STREAMS_SOURCE)

  BACKENDS.each do |backend, flags|
    output = on_pty([KEX, '--no-colors', *flags, terminal], 'hello-from-pty')
    if output.include?('CHILD-TTY') && output.include?('CHILD-GOT:hello-from-pty') && output.include?('EXIT:0')
      puts "PASS: #{backend}: a Process.stream child has the caller's terminal, and reads a line typed there"
    else
      failed = true
      puts "FAIL: #{backend}: expected CHILD-TTY, CHILD-GOT:hello-from-pty and EXIT:0; got #{output.inspect}"
      puts '  CHILD-PIPE means the child was given a pipe instead of the caller\'s descriptors.'
    end

    stdout, stderr, = Open3.capture3(KEX, '--no-colors', *flags, streams, stdin_data: '')
    if stdout.include?('CHILD-OUT') && !stdout.include?('CHILD-ERR') && stderr.include?('CHILD-ERR')
      puts "PASS: #{backend}: a Process.stream child's stderr is the caller's stderr"
    else
      failed = true
      puts "FAIL: #{backend}: expected CHILD-OUT on stdout and CHILD-ERR on stderr only; " \
           "got stdout #{stdout.inspect}, stderr #{stderr.inspect}"
    end
  end
end

exit(failed ? 1 : 0)
