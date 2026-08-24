# Records and Functional Updates

Records are immutable values. Construct one by naming every mandatory field;
fields with defaults and optional fields may be omitted.

```kex
record User do
  name : String
  age : Integer = 0
  active? : Bool = false
end

let ada = User { name: "Ada" }
```

## Updating an existing receiver: `New`

Inside a `make` or `serving` block for a record, `New { ... }` constructs a
copy of `this` with the listed fields replaced. Unlisted fields retain their
current values.

```kex
make User do
  let birthday -> User = New { age: @age + 1 }
end
```

`@age` is shorthand for `this.age`. Conceptually,
`New { age: value }` is `This { ...this, age: value }`.

## Several updates: implicit `new`

Record methods and slots also receive a local named `new`, initially equal to
`this`. Assigning a field rebuilds the record and rebinds this local; it does
not mutate the original value.

```kex
make User do
  let activateAs(name: String) -> User do
    new.name = name
    new.active? = true
    return new
  end
end
```

Use `New { ... }` for one compact update and `new.field = ...` when several
steps or branches are clearer. `new` is an ordinary, shadowable identifier
outside these receiver blocks.

## Constructing from defaults: `This`

`This { ... }` constructs the enclosing record from its declared defaults,
not from the current receiver. Mandatory fields still have to be supplied.

```kex
make User do
  let anonymous -> User = This { name: "anonymous" }
end
```

## Updating any mutable binding

Field assignment on a `var` binding uses the same functional rebinding rule:

```kex
var user = User { name: "Ada" }
user.age = 37
```

The value is replaced by a fresh record. Nested paths such as
`user.profile.name = ...` are not currently supported; update the nested value
explicitly and then assign it back.

Outside a receiver block, record spread provides the general expression form:

```kex
let older = User { ...user, age: user.age + 1 }
```

Entries apply left to right, so later spreads and explicit fields win.
