%% Kex.Intrinsic.Math — BEAM primitive backend for Math.* functions
%% that can't be plain 1:1 BIF forwards (math:exp needs an argument so
%% can't back a 0-arg constant; Erlang's math has no log/2, hypot/2,
%% or cbrt/1). Moved from kex_io where math functions didn't belong.
%% Matching src/interpreter/stdlib/math.cxx exactly.
-module(kex_intrinsic_math).
-export([e/0, log/1, log/2, hypot/2, cbrt/1]).

%% Math.e / Math.E — Euler's number (0-arg constant).
e() -> math:exp(1.0).

%% Math.log(x) — natural logarithm.
log(X) -> math:log(X).

%% Math.log(x, base) — logarithm with arbitrary base (ln x / ln base).
log(X, Base) -> math:log(X) / math:log(Base).

%% Math.hypot(a, b) — Euclidean distance sqrt(a² + b²) avoiding overflow.
hypot(A, B) -> math:sqrt(A * A + B * B).

%% Math.cbrt(x) — cube root.
cbrt(X) when X < 0 -> -math:pow(-X, 1.0 / 3.0);
cbrt(X) -> math:pow(X, 1.0 / 3.0).
