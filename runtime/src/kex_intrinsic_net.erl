-module(kex_intrinsic_net).
-export([port/1, support/0, unsupported/1]).

port(Value) when is_integer(Value), Value >= 0, Value =< 65535 -> {'Ok', {'Net.Port', Value}};
port(_) -> net_error('Parse', 'Connect', <<"port must be between 0 and 65535">>).

support() ->
    Yes = {'Net.SupportValue', true, true},
    No = {'Net.SupportValue', false, false},
    {'Net.SupportReport', Yes, Yes, Yes, Yes, Yes, Yes, Yes, Yes, No}.

unsupported(Operation) -> net_error('UnsupportedBackend', binary_to_atom(Operation, utf8), <<Operation/binary, " is not available on this backend">>).
net_error(Kind, Operation, Message) -> {'Error', {'Net.NetError', Kind, Operation, Message, 'None', 'None'}}.
