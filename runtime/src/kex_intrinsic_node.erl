%% Kex.Intrinsic.Node — distributed Erlang: this node's identity, and the
%% other BEAM nodes it is connected to. Node names are atoms, as in Erlang
%% (Kex spells them `:"b@myhost"`); a cookie arrives as a string.
-module(kex_intrinsic_node).
-export([self/0, 'alive?'/0, list/0, start/1, stop/0, connect/1, disconnect/1,
         'setCookie'/1, send/3, 'whereIs'/2, spawn/2]).

self() -> erlang:node().

'alive?'() -> erlang:is_alive().

list() -> erlang:nodes().

%% `name@host.domain` needs long names; a bare `name` or `name@host` is short.
start(Name) ->
    Domain = case binary:split(atom_to_binary(Name), <<"@">>) of
                 [_, Host] -> case binary:match(Host, <<".">>) of
                                  nomatch -> shortnames;
                                  _ -> longnames
                              end;
                 _ -> shortnames
             end,
    case net_kernel:start(Name, #{name_domain => Domain}) of
        {ok, _} -> {'Ok', erlang:node()};
        {error, {already_started, _}} -> {'Ok', erlang:node()};
        {error, Reason} -> {'Error', iolist_to_binary(io_lib:format("~p", [Reason]))}
    end.

stop() -> net_kernel:stop() =:= ok.

connect(Node) -> net_kernel:connect_node(Node) =:= true.

disconnect(Node) -> erlang:disconnect_node(Node) =:= true.

'setCookie'(Cookie) -> erlang:set_cookie(binary_to_atom(Cookie)), 'Kex.Unit'.

send(Node, Name, Message) ->
    erlang:send({Name, Node}, Message), 'Kex.Unit'.

'whereIs'(Node, Name) ->
    Where = case Node of
                Local when Local =:= node() -> erlang:whereis(Name);
                Remote -> rpc:call(Remote, erlang, whereis, [Name])
            end,
    case Where of
        Pid when is_pid(Pid) -> {'Just', Pid};
        _ -> 'None'
    end.

%% A fun runs as code of the module that defined it, so that module must be
%% loaded on `Node` too. When it is not, ship this node's object code first.
%% A module the remote already has is left alone: it may be a different
%% version that processes there are running.
spawn(Node, Fun) ->
    {module, Module} = erlang:fun_info(Fun, module),
    ensure_remote_module(Node, Module),
    erlang:spawn(Node, Fun).

ensure_remote_module(Node, _Module) when Node =:= node() -> ok;
ensure_remote_module(Node, Module) ->
    case rpc:call(Node, code, is_loaded, [Module]) of
        {file, _} -> ok;
        _ ->
            case code:get_object_code(Module) of
                {Module, Binary, File} ->
                    rpc:call(Node, code, load_binary, [Module, File, Binary]),
                    ok;
                error -> ok
            end
    end.
