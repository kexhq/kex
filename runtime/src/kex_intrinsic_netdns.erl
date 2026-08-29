-module(kex_intrinsic_netdns).
-export([name/1, addresses/1, resolverDefault/0, resolver/1,
         resolverWith/1, resolverAddresses/2, lookup/3, clear/1,
         statistics/1, close/1]).

name(Text) when is_binary(Text), byte_size(Text) > 0, byte_size(Text) =< 253 ->
    Display = strip_dot(Text),
    case kex_intrinsic_uri:idnaHost(Display) of
        {ok, ASCII} -> case valid_name(ASCII) of
            true -> {'Ok', {'Net.DNS.Name', Display, ASCII}};
            false -> error_value('Parse', <<"invalid DNS name">>)
        end;
        error -> error_value('Parse', <<"invalid DNS name">>)
    end;
name(_) -> error_value('Parse', <<"invalid DNS name">>).

addresses(Name = {'Net.DNS.Name', _, _}) ->
    case resolverDefault() of
        {'Ok', Resolver} ->
            Result = resolverAddresses(Resolver, Name), close(Resolver), Result;
        Error -> Error
    end.

resolverDefault() ->
    resolver({'Net.DNS.CacheOptions', 1024, {'Duration', 3600.0},
              {'Duration', 30.0}}).

resolver({'Net.DNS.CacheOptions', Entries, {'Duration', MaximumTtl},
          {'Duration', NegativeTtl}})
  when is_integer(Entries), Entries > 0, is_number(MaximumTtl), MaximumTtl >= 0,
       is_number(NegativeTtl), NegativeTtl >= 0 ->
    start_resolver(Entries, MaximumTtl, NegativeTtl, [], [], 2, 5000);
resolver(_) -> error_value('Parse', <<"invalid DNS cache options">>).

resolverWith({'Net.DNS.ResolverOptions',
              {'Net.DNS.CacheOptions', Entries, {'Duration', MaximumTtl},
               {'Duration', NegativeTtl}}, Nameservers, Search, Retries,
              {'Duration', Timeout}})
  when is_integer(Entries), Entries > 0, is_number(MaximumTtl), MaximumTtl >= 0,
       is_number(NegativeTtl), NegativeTtl >= 0, is_list(Nameservers),
       Nameservers =/= [], is_list(Search), is_integer(Retries), Retries > 0,
       is_number(Timeout), Timeout > 0 ->
    case {nameserver_values(Nameservers), search_values(Search)} of
        {{ok, Servers}, {ok, Domains}} ->
            start_resolver(Entries, MaximumTtl, NegativeTtl, Servers, Domains,
                           Retries, seconds_ms(Timeout));
        _ -> error_value('Parse', <<"invalid DNS resolver options">>)
    end;
resolverWith(_) -> error_value('Parse', <<"invalid DNS resolver options">>).

start_resolver(Entries, MaximumTtl, NegativeTtl, Nameservers, Search, Retries, Timeout) ->
    Query = #{nameservers => Nameservers, search => Search, retries => Retries,
              timeout => Timeout},
    Pid = spawn(fun() -> loop(#{}, Entries, seconds_ms(MaximumTtl),
                              seconds_ms(NegativeTtl), Query, 0, 0, 0, 0) end),
    {'Ok', {'Net.DNS.Resolver', Pid, call_timeout(Query)}}.

resolverAddresses({'Net.DNS.Resolver', Pid, Timeout}, Name) -> call(Pid, {addresses, Name}, Timeout);
resolverAddresses(_, _) -> error_value('Parse', <<"invalid DNS resolver">>).
lookup({'Net.DNS.Resolver', Pid, Timeout}, Kind, Name) -> call(Pid, {lookup, Kind, Name}, Timeout);
lookup(_, _, _) -> error_value('Parse', <<"invalid DNS lookup">>).
clear({'Net.DNS.Resolver', Pid, _}) ->
    case is_process_alive(Pid) of true -> Pid ! clear; false -> ok end, 'Kex.Unit'.
statistics({'Net.DNS.Resolver', Pid, Timeout}) ->
    case call(Pid, statistics, Timeout) of
        {'Error', _} -> {'Net.DNS.CacheStatistics', 0, 0, 0, 0, 0};
        Value -> Value
    end.
close({'Net.DNS.Resolver', Pid, _}) ->
    case is_process_alive(Pid) of true -> Pid ! close; false -> ok end, 'Kex.Unit'.

loop(Cache, Limit, MaxTtl, NegativeTtl, Query, Hits, Misses, NegativeHits, Evictions) ->
    receive
        {call, From, Ref, {lookup, Kind, Name}} ->
            {Result, NextCache, Hit, NegativeHit, Evicted} =
                cached_lookup(Kind, Name, Cache, Limit, MaxTtl, NegativeTtl, Query),
            From ! {Ref, Result},
            loop(NextCache, Limit, MaxTtl, NegativeTtl, Query,
                 Hits + bool(Hit), Misses + bool(not Hit),
                 NegativeHits + bool(NegativeHit), Evictions + Evicted);
        {call, From, Ref, {addresses, Name}} ->
            {V6, Cache1, Hit6, Negative6, Evicted6} =
                cached_lookup('AAAA', Name, Cache, Limit, MaxTtl, NegativeTtl, Query),
            {V4, Cache2, Hit4, Negative4, Evicted4} =
                cached_lookup('A', Name, Cache1, Limit, MaxTtl, NegativeTtl, Query),
            Addresses = address_values(V6) ++ address_values(V4),
            Reply = case Addresses of [] -> error_value('Resolve', <<"DNS name did not resolve">>); _ -> {'Ok', Addresses} end,
            From ! {Ref, Reply},
            loop(Cache2, Limit, MaxTtl, NegativeTtl, Query,
                 Hits + bool(Hit6) + bool(Hit4), Misses + bool(not Hit6) + bool(not Hit4),
                 NegativeHits + bool(Negative6) + bool(Negative4), Evictions + Evicted6 + Evicted4);
        {call, From, Ref, statistics} ->
            From ! {Ref, {'Net.DNS.CacheStatistics', maps:size(Cache), Hits, Misses, NegativeHits, Evictions}},
            loop(Cache, Limit, MaxTtl, NegativeTtl, Query, Hits, Misses, NegativeHits, Evictions);
        clear -> loop(#{}, Limit, MaxTtl, NegativeTtl, Query, Hits, Misses, NegativeHits, Evictions);
        close -> ok;
        _ -> loop(Cache, Limit, MaxTtl, NegativeTtl, Query, Hits, Misses, NegativeHits, Evictions)
    end.

cached_lookup(Kind, Name = {'Net.DNS.Name', _, ASCII}, Cache0, Limit, MaxTtl, NegativeTtl, Query) ->
    Now = erlang:monotonic_time(millisecond), Key = {Kind, ASCII}, Cache = expire(Cache0, Now),
    case maps:find(Key, Cache) of
        {ok, {_, Result, Negative, _}} -> {Result, Cache, true, Negative, 0};
        error ->
            Result = resolve(Kind, Name, Query), Negative = is_error(Result),
            Ttl = case Negative of true -> NegativeTtl; false -> MaxTtl end,
            {Trimmed, Evicted} = make_room(Cache, Limit),
            {Result, maps:put(Key, {Now + Ttl, Result, Negative, Now}, Trimmed), false, false, Evicted}
    end.

resolve(Kind, {'Net.DNS.Name', _, ASCII}, Query) ->
    case kind_atom(Kind) of
        invalid -> error_value('Parse', <<"invalid DNS record type">>);
        a -> resolve_values(Kind, ASCII, a, Query);
        aaaa -> resolve_values(Kind, ASCII, aaaa, Query);
        Type ->
            resolve_values(Kind, ASCII, Type, Query)
    end.

resolve_values(Kind, ASCII, Type, Query) ->
    try lookup_candidates(query_names(ASCII, maps:get(search, Query)), Type, Query) of
        {ok, Values} when Values =/= [] ->
            {'Ok', {'Net.DNS.LookupResponse',
                    [record_value(Kind, Value) || Value <- Values],
                    'Indeterminate'}};
        timeout -> error_value('Timeout', <<"DNS lookup timed out">>);
        _ -> error_value('Resolve', <<"DNS record did not resolve">>)
    catch _:_ -> error_value('Resolve', <<"DNS lookup failed">>) end.

lookup_candidates([], _, _) -> not_found;
lookup_candidates([Name | Rest], Type, Query) ->
    Opts = query_options(Query), Timeout = maps:get(timeout, Query),
    case inet_res:resolve(binary_to_list(Name), in, Type, Opts, Timeout) of
        {ok, Message} ->
            case inet_dns:msg(Message, anlist) of
                [] -> lookup_candidates(Rest, Type, Query);
                Answers -> {ok, [inet_dns:rr(RR, data) || RR <- Answers,
                                  inet_dns:rr(RR, type) =:= Type]}
            end;
        {error, timeout} -> timeout;
        _ -> lookup_candidates(Rest, Type, Query)
    end.

query_options(Query) ->
    Base = [{retry, maps:get(retries, Query)}, {timeout, maps:get(timeout, Query)}],
    case maps:get(nameservers, Query) of [] -> Base; Servers -> [{nameservers, Servers} | Base] end.
query_names(ASCII, Search) ->
    case {binary:match(ASCII, <<".">>), Search} of
        {nomatch, [_ | _]} -> [<<ASCII/binary, ".", Domain/binary>> || Domain <- Search] ++ [ASCII];
        _ -> [ASCII]
    end.

nameserver_values(Values) -> nameserver_values(Values, []).
nameserver_values([], Acc) -> {ok, lists:reverse(Acc)};
nameserver_values([{'Net.DNS.Nameserver', {'Net.IP.Address', Address}, {'Net.Port', Port}} | Rest], Acc)
  when is_binary(Address), is_integer(Port), Port > 0, Port =< 65535 ->
    case inet:parse_address(binary_to_list(Address)) of
        {ok, IP} -> nameserver_values(Rest, [{IP, Port} | Acc]);
        _ -> error
    end;
nameserver_values(_, _) -> error.
search_values(Values) -> search_values(Values, []).
search_values([], Acc) -> {ok, lists:reverse(Acc)};
search_values([{'Net.DNS.Name', _, ASCII} | Rest], Acc) -> search_values(Rest, [ASCII | Acc]);
search_values(_, _) -> error.

kind_atom('A') -> a; kind_atom('AAAA') -> aaaa; kind_atom('CNAME') -> cname;
kind_atom('MX') -> mx; kind_atom('TXT') -> txt; kind_atom('SRV') -> srv;
kind_atom('PTR') -> ptr; kind_atom(_) -> invalid.
record_value('A', IP) -> {'AddressRecord', ip_value(IP)};
record_value('AAAA', IP) -> {'AddressRecord', ip_value(IP)};
record_value('CNAME', Domain) -> {'CanonicalName', name_value(Domain)};
record_value('MX', {Preference, Exchange}) -> {'MailExchange', Preference, name_value(Exchange)};
record_value('TXT', Chunks) -> {'TextRecord', [unicode:characters_to_binary(Chunk) || Chunk <- Chunks]};
record_value('SRV', {Priority, Weight, Port, Target}) -> {'ServiceRecord', Priority, Weight, {'Net.Port', Port}, name_value(Target)};
record_value('PTR', Domain) -> {'PointerRecord', name_value(Domain)}.

ip_value(IP) -> {'Net.IP.Address', iolist_to_binary(inet:ntoa(IP))}.
name_value(Domain) -> Value = strip_dot(unicode:characters_to_binary(Domain)), {'Net.DNS.Name', Value, string:lowercase(Value)}.
address_values({'Ok', {'Net.DNS.LookupResponse', Records, _}}) -> [Address || {'AddressRecord', Address} <- Records];
address_values(_) -> [].

valid_name(Name) -> Labels = binary:split(Name, <<".">>, [global]), Labels =/= [] andalso lists:all(fun valid_label/1, Labels).
valid_label(Label) when byte_size(Label) > 0, byte_size(Label) =< 63 ->
    First = binary:first(Label), Last = binary:last(Label),
    First =/= $- andalso Last =/= $- andalso
    lists:all(fun(C) -> (C >= $a andalso C =< $z) orelse (C >= $0 andalso C =< $9) orelse C =:= $- end, binary:bin_to_list(Label));
valid_label(_) -> false.
expire(Cache, Now) -> maps:filter(fun(_, {Expiry, _, _, _}) -> Expiry >= Now end, Cache).
make_room(Cache, Limit) when map_size(Cache) < Limit -> {Cache, 0};
make_room(Cache, _) ->
    [{Key, _} | _] = lists:sort(fun({_, {_, _, _, A}}, {_, {_, _, _, B}}) -> A =< B end, maps:to_list(Cache)),
    {maps:remove(Key, Cache), 1}.
strip_dot(<<>>) -> <<>>;
strip_dot(Text) -> case binary:last(Text) of $. -> binary:part(Text, 0, byte_size(Text)-1); _ -> Text end.
seconds_ms(Value) -> trunc(Value * 1000).
bool(true) -> 1; bool(false) -> 0.
is_error({'Error', _}) -> true; is_error(_) -> false.
call_timeout(Query) -> maps:get(timeout, Query) * (1 bsl maps:get(retries, Query)) + 1000.
call(Pid, Message, Timeout) ->
    case is_process_alive(Pid) of
        false -> error_value('Closed', <<"DNS resolver is closed">>);
        true -> Ref = make_ref(), Pid ! {call, self(), Ref, Message},
                receive {Ref, Value} -> Value after Timeout -> error_value('Timeout', <<"DNS lookup timed out">>) end
    end.
error_value(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'DNS', Message, 'None', 'None'}}.
