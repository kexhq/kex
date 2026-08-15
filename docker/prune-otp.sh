#!/bin/sh
# Removes the OTP applications Kex itself never loads, for the slim image.
#
# Kex uses Erlang through a fixed set of modules (runtime/src/*.erl), so the
# rest of the distribution is dead weight there: OTP ships ~69 MB and this
# leaves ~26 MB. What stays, and why:
#
#   erts kernel stdlib   the emulator and its base — everything
#   compiler             `erlc`, which `-R` and `--compile` shell out to
#   syntax_tools         the compiler's own dependency
#   crypto               Digest.sha256
#   inets ssl public_key asn1
#                        Http requests (kex_http starts inets and ssl)
#   mnesia               the database Kex programs are expected to reach for
#   odbc                 the SQL bridge, same reason
#
# The slim image is for running Kex. `Erlang.*` interop can reach anything in
# the distribution, so the full images keep OTP intact on purpose; if a
# program borrows an application that is not on this list, that is the image
# to use.
set -eu

keep="erts kernel stdlib compiler syntax_tools crypto asn1 public_key ssl inets mnesia odbc"

lib=$(dirname "$(readlink -f "$(command -v erl)")")/../lib/erlang/lib
[ -d "$lib" ] || lib=/usr/lib/erlang/lib
cd "$lib"

for app in *; do
    name=$(echo "$app" | sed 's/-[0-9].*//')
    case " $keep " in
        *" $name "*) ;;
        *) rm -rf "$app" ;;
    esac
done

# OTP ships the Erlang sources and docs of every application it installs;
# nothing loads them.
rm -rf ./*/src ./*/examples ./*/doc ../erts-*/src ../erts-*/doc 2>/dev/null || true
