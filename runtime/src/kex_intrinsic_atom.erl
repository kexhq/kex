%% Kex.Intrinsic.Atom — atoms to and from their names.
-module(kex_intrinsic_atom).
-export([from/1, existing/1, name/1]).

from(Text) -> binary_to_atom(Text, utf8).

existing(Text) ->
    try binary_to_existing_atom(Text, utf8) of
        Atom -> {'Just', Atom}
    catch error:badarg -> 'None'
    end.

name(Atom) -> atom_to_binary(Atom, utf8).
