%% Kex.Intrinsic.Digest — SHA-256 with a stable lowercase-hex boundary.
-module(kex_intrinsic_digest).
-export([sha256/1, 'fileSha256'/1]).

sha256({'Binary', Content}) -> {'Binary', crypto:hash(sha256, Content)};
sha256(Content) -> hex(crypto:hash(sha256, Content)).

'fileSha256'(Path) ->
    case file:read_file(unicode:characters_to_list(Path)) of
        {ok, Content} -> {'Just', sha256(Content)};
        {error, _} -> 'None'
    end.

hex(Bytes) ->
    list_to_binary([io_lib:format("~2.16.0b", [Byte]) || <<Byte>> <= Bytes]).
