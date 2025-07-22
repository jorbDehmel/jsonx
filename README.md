
# `JSONX / jsx`

("Jay-sonks" or "JSON-ex")

A constraint/query data language based on JSON (of which it is a
superset). This implementation is for typescript. JSONX is a
hierarchical data language (but is notably Turing complete). It
allows lambda functions, external interfacing, and code re-use
in a JSON-like form factor.

JSONX Syntax highlighting for VSCode can be found
[here](https://github.com/jorbDehmel/jsonx-highlighting).

## Containerization and Testing

To enter a `Docker` container, run `make docker`. For `podman`,
run `make podman`. Either inside of these containers or on a
suitable Linux environment, run `make test` to run the test
suite.

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

## Types

There are only three types in JSONX: Objects, blobs/strings, and
lambdas. Objects have members, blobs are copy-on-write literals,
and lambdas are callable objects (either internal or external).

Although the `[ ... ]` ("array") syntax may seem different from
the `{ ... }` ("object") syntax, they are actually the same.
Indeed, both `{ 1, 2, 3 }` and `[ a: 1, b: 2, c: 3 ]` are
perfectly legal. In the former case, the numbers are simply
added without any names, accessible only by their indices.
Indices work on arrays or non-arrays.

```js
{
    a: { 4, 5, 6 },
    b: [ a: 1, b: 2, c: 3 ],
    c: a.0, // Resolves to "4"
    d: b.2, // Resolves to "3"
}
```

Similarly, although `"false"` and `false` may seem different,
they are actually the same thing; A string literal with the
value "false". This extends to numbers: `123.456` actually
becomes a string literal with the value "123.456". Even keywords
are really just string literals with special interpretations.

## Lambdas

JSONX has functions in the form of lambdas similar to JS's arrow
functions.

```js
foo: argument => body,
```

The lambda calculus statement $\lambda x . M$ translates to the
JSONX `x => M`. Like lambda calculus, all JSONX lambdas can have
only one argument. If you want to have several, you have several
options: Either [Curry](https://en.wikipedia.org/wiki/Currying)
your functions or assume that your argument has some named
members (the prefered option).

Currying:

```js
// A fn that applies it first argument twice on its second
applyTwiceOn: f => x => f(f(x)),

// To call:
value: applyTwiceOn(env.math.tan)(100.0),
```

Querying:

```js
// A fn that applies it first argument twice on its second
applyTwiceOn: args => args.f(args.f(args.x)),

// To call:
value: applyTwiceOn({f: env.math.tan, x: 100.0}),
```

## Keywords

There are a few keywords for referencing different regions of
the hierarchy.

`this` refers to the current object.

`parent` refers to the parent of the current object. If none
exists, the behaviour is undefined.

`global` refers to the object which has no parent.

`env` is a special static object which is accessible by both
JSONX programs and the instantiating JS/TS program. This is how
JSONX programs are able to access external functions and values:
For instance, `env.math` is a JSONX port of JS's `Math` object.

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

## EBNF

JSONX is *not* parsed via EBNF: Here is a rough approximation
anyways.

```c
JSONX:
      "{" ENTRIES "}"
    | "[" ENTRIES "]"
    | name
    | PATH
    | OBJ "(" OBJ ")"
    | OBJ "=>" OBJ
    ;

ENTRY:
      name ":" JSONX
    | name WEIGHTING ":" JSONX
    | JSONX
    ;

WEIGHTING:
      "?"
    | "!"
    | WEIGHTING WEIGHTING
    ;

ENTRIES:
      ENTRY
    | ENTRIES ","
    | ENTRIES ";"
    ;

PATH:
      name
    | PATH "." name
    ;
```

## Turing-Completeness and Space/Time Limitting

Because JSONX is a superset of lambda calculus, it is
Turing-complete. This makes it **unsuitable** for unregulated
data transmission, but suitable for verified or local data. If
you must process an unverified JSONX document, the parser has a
few safeguards.

```ts
const a = JSONX.loadf('foo.jsx',
    5_000,  // Max milliseconds to spend
    128_000 // Max bytes of memory to spend
);

const b = JSONX.loads('...',
    5_000,  // Same as above
    128_000 // Same as above
);
```

If the parsing process exceeds the given boundaries, an error
will be thrown. If no boundaries are provided,
**none are applied**.

## License

Warren MacEvoy, 2025, MIT License
