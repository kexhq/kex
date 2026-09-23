%% Kex.Intrinsic.Atom — atoms to and from their names.
-module(kex_intrinsic_atom).
-export([from/1, name/1]).

from(Text) -> binary_to_atom(Text, utf8).

name(Atom) -> atom_to_binary(Atom, utf8).
