# Mutability

## Immutable by Default

All bindings created with `let` are immutable:

```kex
let name = "Akos"
name = "Other"   # compile error
```

## Local Variables

`var` creates a mutable binding. Inside a function body, this is still pure from the caller's perspective:

```kex
let sort(list: [Int]) -> [Int] do
  var result = list
  # ... mutate result ...
  return result
end
```

## The `!` Operator

Calling `method!` on a `var` reassigns the result back to the variable:

```kex
var list = [1, 2, 3, 4, 5]
list.push!(6)          # list is now [1, 2, 3, 4, 5, 6]
list.filter!(&.even?)  # list is now [2, 4, 6]
```

Rules:
- `!` can only be called on `var` bindings — compile error on `let`
- Sugar for `x = x.method(args)`
- Any function that returns the same type as `this` can use `!`
- Compiler can optimize to in-place mutation when sole owner

```kex
# These are equivalent:
ages.put!("alice", 33)
ages = ages.put("alice", 33)
```

## Purity Rules for var

- `var` inside function body — pure (mutation doesn't escape)
- `var` in process state (across `receive` cycles) — foul

```kex
# Pure — local var
let countErrors(lines: [String]) -> Int do
  var count = 0
  lines.each { |l| count = count + 1 if l.contains?("ERROR") }
  return count
end

# Foul — process state
foul counter = spawn do
  var state = 0
  loop do
    receive do
      :increment -> state = state + 1
    end
  end
end
```

## Closure Capture

Closures capture the value at creation time, not a reference:

```kex
var x = 10
let snap = { x }   # captures 10
x = 20
snap()              # returns 10
```

No shared mutable state through closures. Use processes for shared state.
