%% Kex.Intrinsic.Math — BEAM primitive backend for Math.* functions
%% that can't be plain 1:1 BIF forwards (math:exp needs an argument so
%% can't back a 0-arg constant; Erlang's math has no log/2, hypot/2,
%% or cbrt/1). Moved from kex_io where math functions didn't belong.
%% Matching src/interpreter/stdlib/math.cxx exactly.
-module(kex_intrinsic_math).
-export([e/0, pi/0, sqrt/1, cbrt/1, sin/1, cos/1, tan/1,
         asin/1, acos/1, atan/1, atan2/2, sinh/1, cosh/1, tanh/1,
         log/1, log/2, log2/1, log10/1, exp/1, pow/2,
         abs/1, floor/1, ceil/1, hypot/2]).

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

pi() -> math:pi().
sqrt(X) -> math:sqrt(X).
sin(X) -> math:sin(X).
cos(X) -> math:cos(X).
tan(X) -> math:tan(X).
asin(X) -> math:asin(X).
acos(X) -> math:acos(X).
atan(X) -> math:atan(X).
atan2(Y, X) -> math:atan2(Y, X).
sinh(X) -> math:sinh(X).
cosh(X) -> math:cosh(X).
tanh(X) -> math:tanh(X).
log2(X) -> math:log2(X).
log10(X) -> math:log10(X).
exp(X) -> math:exp(X).
pow(X, Y) -> math:pow(X, Y).
abs(X) -> erlang:abs(X).
floor(X) -> erlang:floor(X).
ceil(X) -> erlang:ceil(X).
