/**
 * @brief Defines the JSONX class, with static utilities
 */

import {PathOrFileDescriptor, readFileSync} from "fs";

import {JSONXBlob} from "./blob";
import {Pos, tokenize} from "./lexer";

/// The type of an entry in a JSONX object
export type JSONXVar = JSONX|JSONXBlob|JSONXLambdaBody;

/// A single entry in an object: Contains name, value, and
/// weight
class Entry {
  /// The value owned by this entry
  value: JSONXVar;

  /// The weighting of this entry
  weight: number = 0;

  /// The name of this entry: This may not be unique!
  name: string;
}

/// Returns whether an entry is a number
function isNumber(data: unknown): data is number {
  // "statically typed language" my foot
  return typeof data === 'number';
}

/// A lambda body which can be evaluated
class JSONXLambdaBody {
  /// The capture variable: The name to be replaced in the body
  /// with the passed argument upon call
  argName: String;

  /// The body which will be operated on when this object is
  /// called
  body: Pos|((thisJSONX: JSONXVar, arg: JSONXVar) => JSONXVar);

  /// Creates a new lambda from capture name and body (where the
  /// body can be an arbitrary external function)
  constructor(argName: String,
              body: Pos|((thisJSONX: JSONXVar,
                          arg: JSONXVar) => JSONXVar)) {
    this.argName = argName;
    this.body = body;
  }

  ///
  replace(key: String, value: Pos): JSONXVar {
    if (!(this.body instanceof Function)) {
      return new JSONXLambdaBody(this.argName,
                                 this.body.replace(key, value));
    }
    return new JSONXLambdaBody(this.argName, this.body);
  }

  /// Return a string representation of this object
  stringify(_: string = ""): string {
    let out = `${this.argName} => `;
    if (this.body instanceof Function) {
      out += "...";
    } else {
      let first = true;
      for (const tok of this.body.tokens) {
        if (first) {
          first = false;
        } else {
          out += ' ';
        }
        out += tok.text;
      }
    }
    return out;
  }

  /// Dummy wrapper for typechecking: Always throws when called.
  get(_: string): JSONXVar {
    throw new Error("Expected JSONX, but saw lambda body");
  }

  /// Return the body, but with the named capture replaced by
  /// `arg`. `thisJSONX` is used in external calls for when we
  /// need to capture the calling scope.
  call(thisJSONX: JSONX, arg: Pos|JSONXVar): JSONXVar {
    let realArg: Pos;
    if (arg instanceof Pos) {
      realArg = arg;
    } else {
      realArg = new Pos(tokenize(arg.stringify()));
    }

    if (this.body instanceof Function) {
      // External call
      return this.body(thisJSONX,
                       JSONX.parseObject(realArg, thisJSONX));
    } else {
      // Internal call
      return JSONX.parseObject(
          this.body.replace(this.argName, realArg), thisJSONX);
    }
  }
}

/**
 * @brief A queryable object. Similar to a standard JSON object,
 * but contains lambdas, references, etc. to reduce code reuse
 */
export class JSONX {
  /// Available to all JSONX objects via the `env` keyword. This
  /// is where all external interfacing occurs
  static env = new JSONX();

  /**
   * @brief Load from a string
   */
  static loads(text: string, maxMs: number = 5_000,
               maxBytesDA: number = 128_000,
               filepath?: PathOrFileDescriptor): JSONX {
    // Set max bytes
    JSONXBlob.maxBytes = maxBytesDA;

    // If a max time was given, start a timer
    let timeoutID: NodeJS.Timeout|undefined = undefined;
    if (maxMs != undefined) {
      timeoutID = setTimeout(() => {
        throw Error(
            `Exceeded loadsJSONX time limit of ${maxMs} ms`);
      }, maxMs);
    }

    // Lex
    const tokens = tokenize(text, filepath ? filepath.toString()
                                           : undefined);

    // Parse
    if (tokens.length == 0) {
      if (timeoutID != undefined) {
        clearTimeout(timeoutID);
      }
      return undefined;
    }

    let pos = new Pos(tokens);
    let parsed = new JSONX(pos);

    // If we have a timer running, cancel it
    if (timeoutID != undefined) {
      clearTimeout(timeoutID);
    }
    return parsed.get(0) as JSONX;
  }

  /// Load from some file descriptor
  static loadf(filepath: PathOrFileDescriptor,
               maxMs: number = 5_000,
               maxBytesDA: number = 128_000): JSONX {
    // Load file contents
    const text = readFileSync(filepath).toString();
    return JSONX.loads(text, maxMs, maxBytesDA, filepath);
  }

  /// Return a string representation of this object
  stringify(tabbing: string = ""): string {
    if (tabbing.length > 20) {
      return "OVERTABBED";
    }

    let out = "{\n";
    for (const entry of this.variables) {
      out += `${tabbing}  ${entry.name}`;
      if (entry.weight < 0) {
        out += '?'.repeat(-entry.weight);
      } else if (entry.weight > 0) {
        out += '!'.repeat(entry.weight);
      }
      out += ": ";
      if (entry.value) {
        out += entry.value.stringify(tabbing + "  ");
      } else {
        out += "UNDEFINED";
      }
      out += ",\n";
    }
    out += tabbing + "}";
    return out;
  }

  //////////////////////////////////////////////////////////////

  /// If provided, the scope surrounding this one
  private parent?: JSONX;

  /// If not provided, same as TS's `this`
  private thisJSONX?: JSONX;

  /// The token stream this scope uses. Deleted after use
  private contents: Pos;

  /// Once resolved, contains the entries of this scope
  private members: Entry[] = [];

  /// True iff we have resolved all member LHS-es
  private isResolved: boolean = false;

  //////////////////////////////////////////////////////////////

  /// Advances the number until it points to the last token of
  /// an object, logging the object in this.contents.
  private parseEntry(): void {
    // Set up default values (no name or weight)
    let name: string = this.members.length.toString();
    let weight = 0;

    // If provided, parse weight (multiple colons are allowed)
    if (this.contents.peek().text == ":" ||
        this.contents.peek().text == "!" ||
        this.contents.peek().text == "?") {
      name = this.contents.cur().text;
      this.contents.next();

      while (this.contents.cur().text == "?") {
        --weight;
        this.contents.next();
      }
      while (this.contents.cur().text == "!") {
        ++weight;
        this.contents.next();
      }
      while (this.contents.cur().text == ":") {
        this.contents.next();
      }
    }

    // Parse value
    let value = JSONX.parseObject(this.contents, this);

    // Push key-weight-value
    this.members.push(
        {name : name, value : value, weight : weight});

    // Ignore any commas or semicolons
    while (this.contents.cur().text == "," ||
           this.contents.cur().text == ";") {
      this.contents.next();
    }
  }

  /// Parses (NOT recursively) and object so that it can later
  /// be queried and resolved.
  static parseObject(contents: Pos, context?: JSONX): JSONXVar {
    let value: JSONXVar;
    if (contents.cur().text == "{") {
      // Object
      let stack: string[] = [ "{" ];

      contents.next();
      const first = contents.tell();
      while (stack.length != 0) {
        if (contents.cur().text == "{") {
          stack.push("{");
        } else if (contents.cur().text == "}") {
          if (stack.pop() == "[") {
            throw new Error("Expected ']', but saw '}'");
          }
        } else if (contents.cur().text == "[") {
          stack.push("[");
        } else if (contents.cur().text == "]") {
          if (stack.pop() == "}") {
            throw new Error("Expected '}', but saw ']'");
          }
        }
        contents.next();
      }
      const first_after = contents.tell() - 1;

      value = new JSONX(contents.child(first, first_after),
                        context);
    } else if (contents.cur().text == "[") {
      // Array
      let stack: string[] = [ "[" ];

      contents.next();
      const first = contents.tell();
      while (stack.length != 0) {
        if (contents.cur().text == "{") {
          stack.push("{");
        } else if (contents.cur().text == "}") {
          if (stack.pop() == "[") {
            throw new Error("Expected ']', but saw '}'");
          }
        } else if (contents.cur().text == "[") {
          stack.push("[");
        } else if (contents.cur().text == "]") {
          if (stack.pop() == "}") {
            throw new Error("Expected '}', but saw ']'");
          }
        }
        contents.next();
      }
      const firstAfter = contents.tell() - 1;

      value =
          new JSONX(contents.child(first, firstAfter), context);
    } else {
      // Literal
      value = context.get(contents.cur().text);
      if (value == undefined) {
        value = new JSONXBlob();
        value.set(JSONXBlob.encode(contents.cur().text));
      }
      contents.next();
    }

    // Math, lambdas and calls thereof can be here
    let keepLooking = true;
    while (keepLooking) {
      keepLooking = false;
      if (contents.cur().text == "(") {
        // Lambda call
        if (!(value instanceof JSONXLambdaBody)) {
          console.log(value.stringify());
          throw new Error("Cannot call non-lambda");
        }

        // Advance past open paren
        contents.next();

        // Parse arg object
        const startPos = contents.tell();
        JSONX.parseObject(contents, context);
        const firstAfter = contents.tell();
        let arg = contents.child(startPos, firstAfter);

        // Advance past close paren
        if (contents.cur().text != ")") {
          throw new Error(
              "Missing lambda call closing parenthesis");
        }
        contents.next();

        // Replace w/ call
        value = value.call(context, arg);
        keepLooking = true;
      } else if (contents.cur().text == "=>") {
        // Lambda definition
        // Value is retroactively the argument name
        contents.next();

        const startVal = contents.tell();
        JSONX.parseObject(contents, context);
        const firstAfter = contents.tell();

        value = new JSONXLambdaBody(
            value.stringify(),
            contents.child(startVal, firstAfter));
        keepLooking = true;
      } else if (contents.peek(1).type == "MATH") {
        throw new Error('Math is unimplemented');
        keepLooking = true;
      } else if (contents.cur().text == ".") {
        // Path to be resolved
        contents.next();
        let name = contents.cur().text;
        contents.next();
        value = value.get(name);
        keepLooking = true;
      }
    }

    return value;
  }

  //////////////////////////////////////////////////////////////

  /// Initialize from some token stream
  constructor(contents: Pos = new Pos([]), parent?: JSONX,
              thisJSONX?: JSONX) {
    this.parent = parent;
    this.contents = contents;
    this.thisJSONX = thisJSONX;
  }

  /// Get the number of members
  get length(): number {
    return this.members.length;
  }

  /// Get the keys of members
  get variables(): Entry[] {
    this.ensureResolved();
    return this.members;
  }

  /// If we have not resolved, do so. Otherwise, do nothing
  private ensureResolved() {
    if (!this.isResolved) {
      // Resolve
      this.members = [];
      this.contents.seek(0);
      this.isResolved = true;
      while (!this.contents.done()) {
        this.parseEntry();
      }
    }
  }

  /// Get the object with the given identifier (or undefined)
  get(name: string|number): JSONXVar|undefined {
    name = name.toString();

    // Keywords
    if (name == "this") {
      return this.thisJSONX ?? this;
    } else if (name == "parent") {
      return this.parent;
    } else if (name == "global") {
      if (this.parent) {
        return this.parent.get(name);
      } else {
        return this;
      }
    } else if (name == "env") {
      return JSONX.env;
    }

    // Ensure our body has been parsed
    this.ensureResolved();

    // Locate the thing we need
    let out: Entry|undefined = undefined;
    this.members
        .filter((value) => {
          return value.name == name;
        })
        .forEach((value) => {
          if (out == null || out.weight < value.weight) {
            out = value;
          }
        });
    if (out == undefined) {
      return undefined;
    }
    return out.value;
  }

  /// Add an UNPARSED entry
  add(value: JSONXVar, name?: string,
      weight: number = 0): JSONXVar {
    // We must parse our body to append to it
    this.ensureResolved();
    if (name == null) {
      name = this.members.length.toString();
    }

    // Append and return
    this.members.push(
        {name : name, value : value, weight : weight});
    return this.members[this.members.length - 1].value;
  }
}

/// Load a file as a scope
JSONX.env.add(new JSONXLambdaBody('path', (_, arg) => {
                const pathStr = (arg as JSONXBlob).getString()!;
                const out = JSONX.loadf(
                    pathStr.substring(1, pathStr.length - 1));
                if (!Array.isArray(out)) {
                  return out;
                } else {
                  return undefined;
                }
              }), 'loadf');

/// Load a file as a raw blob
JSONX.env.add(new JSONXLambdaBody('path', (_, arg) => {
                let contents = arg as JSONXBlob;
                const path = contents.getString();
                contents.set(readFileSync(
                    path.substring(1, path.length - 1)));
                return contents;
              }), 'rawf');

/// Format string utility
JSONX.env.add(
    new JSONXLambdaBody('formatString', (thisJSONX, arg) => {
      let contents = arg as JSONXBlob;
      const formatString = contents.getString();

      let out = '';
      for (let i = 0; i + 1 < formatString.length; ++i) {
        if (formatString[i] == '$' &&
            formatString[i + 1] == '{') {
          // Scope out
          let end = i + 2;
          while (formatString[end] != '}') {
            ++end;
          }
          out += thisJSONX.get(formatString.substring(i, end));
          i = end + 1;
        } else {
          out += formatString[i];
        }
      }

      contents.set(JSONXBlob.encode(out));

      return contents;
    }), 'format');

/// Localize some object
JSONX.env.add(
    new JSONXLambdaBody('path_or_jsonx', (context, arg) => {
      if (!(context instanceof JSONX)) {
        throw new Error("'include' must be called within an " +
                        "array or scope");
      } else if (arg instanceof JSONXLambdaBody) {
        throw new Error(
            "Cannot use lambda body as argument to 'include'");
      } else if (arg instanceof JSONXBlob) {
        // Filepath to open, then localize
        return (JSONX.env.get("include") as JSONXLambdaBody)
            .call(context,
                  (JSONX.env.get("loadf") as JSONXLambdaBody)
                      .call(context, arg));
      } else {
        // JSONX to localize
        for (let i = 0; i < arg.length; ++i) {
          const variable = arg.variables.at(i);
          context.add(variable.value, variable.name,
                      variable.weight);
        }
        let toReturn = new JSONXBlob();
        toReturn.set(JSONXBlob.encode('true'));
        return toReturn;
      }
    }), 'include');

let math = JSONX.env.add(new JSONX(), "math") as JSONX;

["E", "LN10", "LN2", "LOG2E", "LOG10E", "PI", "SQRT1_2",
 "SQRT2", "abs", "acos", "asin", "atan", "ceil", "cos", "exp",
 "floor", "log", "max", "min", "pow", "round", "sin", "sqrt",
 "tan"]
    .forEach((name) => {
      if (isNumber(Math[name] as any)) {
        // Raw numbers
        let toAdd = new JSONXBlob();
        toAdd.set(JSONXBlob.encode(Math[name].toString()));
        math.add(toAdd, name);
      } else if (name == "max" || name == "min") {
        // Array-input functions
        math.add(new JSONXLambdaBody('arg', (_, arg) => {
                   let input: number[] = [];
                   for (let i = 0; i < (arg as JSONX).length;
                        ++i) {
                     input.push(Number.parseInt(
                         ((arg as JSONX).get(i) as JSONXBlob)
                             .getString()));
                   }
                   let out = new JSONXBlob();
                   out.set(JSONXBlob.encode(
                       ((Math[name] as any)(input) as Number)
                           .toString()));
                   return out;
                 }), name);
      } else if (name == "pow") {
        // Two-argument function
        // env.math.pow({base: 123, exp: 123})
        math.add(
            new JSONXLambdaBody('arg', (_, arg) => {
              const base = Number.parseInt(
                  ((arg as JSONX).get("base") as JSONXBlob)
                      .getString());
              const exp = Number.parseInt(
                  ((arg as JSONX).get("exp") as JSONXBlob)
                      .getString());
              let out = new JSONXBlob();
              out.set(JSONXBlob.encode(
                  ((Math[name] as any)(base, exp) as Number)
                      .toString()));
              return out;
            }), name);
      } else {
        // Single-argument functions
        math.add(new JSONXLambdaBody('arg', (_, arg) => {
                   const x = Number.parseInt(
                       (arg as JSONXBlob).getString());
                   let out = new JSONXBlob();
                   out.set(JSONXBlob.encode(
                       ((Math[name] as any)(x) as Number)
                           .toString()));
                   return out;
                 }), name);
      }
    });

export {JSONXBlob, JSONXLambdaBody};
