
# `jsx`

A constraint/query data language based on JSON (of which it is a
superset).

## Language

```js
[
    {
        "a":    123,
        "b":    this."a", // `this` is a keyword for this object
        "subscope": {
            "a":    parent.a, // `parent` is the superscope
            b:      321 // Quotes on names are optional
        }
    }, /* Comment */ {
        loadf: env.loadf, /* `env` is a special scope */
        // ? decreases precedence, while ! increases it
        test_1?: loadf("./tests/files/test_1.jsonx"),
        data: test_1.subscope.b
    }, {
        atan: env.math.atan, // Localizing an external lambda fn
        atan_out: atan(1),
        local_e: env.math.E, // A number constant
        exponentiated: env.math.pow({base: 2, exp: 10})
        // Lambda calls can only take 1 argument
    }, {
        // `include` brings all items from some JSONX to the
        // current scope
        _: env.include(env.math),
        val: acos(1)
    }
]
```

A snippet of JSONX code constructs a queryable object. Any
actual values are not computed until requested.

## Usage

```ts
import {JSONX} from "jsonx";

const obj: JSONX = JSONX.loads(
    `{
        atan: env.math.atan,
        out: atan(1)
    }`
);

console.log(obj.get("out"));

/*
- Query "out"
    - "out" is the result of a call to "atan" with argument "1"
        - Query "atan"
            - "atan" is an alias to env.math.atan
            - Query env.math.atan
                - env is the special external object, math.atan
                    is an externally defined lambda therein.
                    Return that lambda.
        - Query "1"
            - "1" is a literal: Return that literal
    - Return "atan" called on "1"
*/
```

## How a Query Works

Suppose we have the document

```js
obj: {
    foo: "Hi",
    fizz: [
        this,
        parent.foo,
        this.fizz
    ],
    fn: x => this.foo,
    call: fn(123)
}
```

Once loaded, the object looks like this:

```js
/* Unresolved */
```

If we queries `obj.fizz.1`, the following intermediate steps
would be taken:

```js
// Step 1: We want obj, so resolve that
obj: {
    foo: /* Unresolved */,
    fizz: /* Unresolved */,
    fn: /* Unresolved */,
    call: /* Unresolved */
}

// Step 2: We want obj.fizz, so resolve that
obj: {
    foo: /* Unresolved */,
    fizz: [
        /* Unresolved */,
        /* Unresolved */,
        /* Unresolved */
    ],
    fn: /* Unresolved */,
}

// Step 3: We want obj.fizz.1, so resolve that
obj: {
    foo: /* Unresolved */,
    fizz: [
        /* Unresolved */,
        parent.foo,
        /* Unresolved */
    ],
    fn: /* Unresolved */,
}

// Our new objective: obj.fizz.1.parent.foo
// This reduces to obj.foo

// Step 4: We want obj, so resolve that (no work needed!)
obj: {
    foo: /* Unresolved */,
    fizz: [
        /* Unresolved */,
        parent.foo,
        /* Unresolved */
    ],
    fn: /* Unresolved */,
}

// Step 5: We want obj.foo, so resolve that
obj: {
    foo: "Hi",
    fizz: [
        /* Unresolved */,
        parent.foo,
        /* Unresolved */
    ],
    fn: /* Unresolved */,
}

// Our new objective: "Hi"
// Since obj."Hi" doesn't exist, the answer is the string "Hi"
```

The process of "resolving" a JSONX block amounts to finding all
entries held directly within it. Once a block is resolved, later
queries do not have to resolve it.

## License

Warren MacEvoy, 2025, MIT License
