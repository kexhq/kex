# Static file server for `make web-demo`, serving the directory it is started
# in. Ruby's own one-liner (`ruby -run -e httpd`) is not an option: WEBrick
# stopped being a default gem in Ruby 3.0, so it would mean a `gem install` on
# every machine that wants the demo. Nothing below needs a gem.
#
# `.wasm` is the reason a plain "serve this directory" is not quite enough:
# browsers reject `WebAssembly.instantiateStreaming` unless the response is
# `application/wasm`, so the MIME table is load-bearing rather than cosmetic.

require "socket"

ROOT = Dir.pwd
PORT = Integer(ARGV[0] || 8743)

MIME = {
  ".html" => "text/html; charset=utf-8",
  ".js"   => "text/javascript; charset=utf-8",
  ".mjs"  => "text/javascript; charset=utf-8",
  ".css"  => "text/css; charset=utf-8",
  ".json" => "application/json; charset=utf-8",
  ".map"  => "application/json; charset=utf-8",
  ".wasm" => "application/wasm",
  ".svg"  => "image/svg+xml",
  ".png"  => "image/png",
  ".ico"  => "image/x-icon",
  ".kex"  => "text/plain; charset=utf-8",
}.freeze

def send_response(socket, status, body, type: "text/plain; charset=utf-8", head: false)
  socket.write("HTTP/1.1 #{status}\r\n" \
               "Content-Type: #{type}\r\n" \
               "Content-Length: #{body.bytesize}\r\n" \
               "Connection: close\r\n" \
               "\r\n")
  socket.write(body) unless head
  status
end

# The browser opens several connections at once for one page — a single
# accept loop would serialise them and stall the demo on the first fetch.
def handle(socket)
  request = socket.gets
  return if request.nil?

  method, target, = request.split
  # Headers go unread otherwise, and closing with data still in the socket
  # gets the client an RST instead of the response.
  loop { line = socket.gets; break if line.nil? || line == "\r\n" }

  unless ["GET", "HEAD"].include?(method)
    return send_response(socket, "405 Method Not Allowed", "method not allowed")
  end

  path = target.to_s.split("?", 2).first.to_s
  path = "/" if path.empty?
  path = path.gsub(/%([0-9A-Fa-f]{2})/) { Regexp.last_match(1).hex.chr }

  file = File.expand_path(File.join(ROOT, path))
  # expand_path has already collapsed any `..`, so this is the whole guard.
  unless file == ROOT || file.start_with?(ROOT + File::SEPARATOR)
    return send_response(socket, "403 Forbidden", "forbidden")
  end
  file = File.join(file, "index.html") if File.directory?(file)

  return send_response(socket, "404 Not Found", "not found") unless File.file?(file)

  send_response(socket, "200 OK", File.binread(file),
                type: MIME.fetch(File.extname(file).downcase, "application/octet-stream"),
                head: method == "HEAD")
end

server = TCPServer.new("127.0.0.1", PORT)

begin
  loop do
    client = server.accept
    Thread.new(client) do |socket|
      begin
        handle(socket)
      rescue Errno::EPIPE, Errno::ECONNRESET
        # The browser gave up on a fetch; not worth a backtrace.
      ensure
        socket.close
      end
    end
  end
rescue Interrupt
  # Ctrl-C is how this is meant to end, so exit quietly rather than with a
  # backtrace from deep inside accept.
  exit 0
end
