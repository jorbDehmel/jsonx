
# `jsonx`

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

## License

Warren MacEvoy, 2025, MIT License
