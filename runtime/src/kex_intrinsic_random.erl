%% Kex.Intrinsic.Random — the OS CSPRNG behind Random.secureBytes
%% (kexhq/kex#404). The seeded generators in random.kex are plain Kex.
-module(kex_intrinsic_random).
-export([secureBytes/1]).

secureBytes(Count) when is_integer(Count), Count > 0 ->
    {'Binary', crypto:strong_rand_bytes(Count)};
secureBytes(_) ->
    {'Binary', <<>>}.
