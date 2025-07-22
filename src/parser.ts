/**
 * @brief
 */

import {PathOrFileDescriptor, readFileSync} from "fs";

import {BlobInstance, BlobManager} from "./blob_manager";
import {Pos, tokenize} from "./lexer";

/// The type of an entry in a JSONX object
export type JSONXVarType = JSONX|BlobInstance|JSONXLambdaBody;

/// A single entry in an object: Contains name, value, and
/// weight
class Entry {
  /// The value owned by this entry
  value: JSONXVarType;

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
  body: JSONXVarType|((thisJSONX: JSONXVarType,
                       arg: JSONXVarType) => JSONXVarType);

  /// Creates a new lambda from capture name and body (where the
  /// body can be an arbitrary external function)
  constructor(argName: String, body: JSONXVarType|
              ((thisJSONX: JSONXVarType,
                arg: JSONXVarType) => JSONXVarType)) {
    this.argName = argName;
    this.body = body;
  }

  /// Return a string representation of this object
  stringify(tabbing: string = ""): string {
    let out = `${this.argName} => `;
    if (this.body instanceof Function) {
      out += "...";
    } else {
      out += this.body.stringify(tabbing);
    }
    return out;
  }

  ///
  get(name: string): JSONXVarType {
    throw new Error("Expected JSONX, but saw lambda body");
  }

  /// Return the body, but with the named capture replaced by
  /// `arg`. `thisJSONX` is used in external calls for when we
  /// need to capture the calling scope.
  call(thisJSONX: JSONXVarType,
       arg: JSONXVarType): JSONXVarType {
    if (this.body instanceof Function) {
      // External call
      return this.body(thisJSONX, arg);
    } else {
      // Internal call
      throw new Error("Lambda replacement is unimplemented");
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

  /** */
  static loads(text: string, maxMs: number = 5_000,
               maxBytesDA: number = 128_000,
               filepath?: PathOrFileDescriptor): JSONX {
    // Set max bytes
    BlobManager.maxBytes = maxBytesDA;

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

  ///
  static loadf(filepath: PathOrFileDescriptor,
               maxMs: number = 5_000,
               maxBytesDA: number = 128_000): JSONX {
    // Load file contents
    const text = readFileSync(filepath).toString();
    return JSONX.loads(text, maxMs, maxBytesDA, filepath);
  }

  ///
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
      out +=
          ": " + entry.value.stringify(tabbing + "  ") + ",\n";
    }
    out += tabbing + "}";
    return out;
  }

  //////////////////////////////////////////////////////////////

  ///
  private parent?: JSONX;

  /// If not provided, same as TS's `this`
  private thisJSONX?: JSONX;

  ///
  private contents: Pos;

  ///
  private members: Entry[] = [];

  ///
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
    let value = this.parseObject();

    // Push key-weight-value
    this.members.push(
        {name : name, value : value, weight : weight});

    // Ignore any commas or semicolons
    while (this.contents.cur().text == "," ||
           this.contents.cur().text == ";") {
      this.contents.next();
    }
  }

  ///
  private parseObject(): JSONXVarType {
    let value: JSONXVarType;
    if (this.contents.cur().text == "{") {
      // Object
      let stack: string[] = [ "{" ];

      this.contents.next();
      const first = this.contents.tell();
      while (stack.length != 0) {
        if (this.contents.cur().text == "{") {
          stack.push("{");
        } else if (this.contents.cur().text == "}") {
          if (stack.pop() == "[") {
            throw new Error("Expected ']', but saw '}'");
          }
        } else if (this.contents.cur().text == "[") {
          stack.push("[");
        } else if (this.contents.cur().text == "]") {
          if (stack.pop() == "}") {
            throw new Error("Expected '}', but saw ']'");
          }
        }
        this.contents.next();
      }
      const first_after = this.contents.tell() - 1;

      value = new JSONX(this.contents.child(first, first_after),
                        this);
    } else if (this.contents.cur().text == "[") {
      // Array
      let stack: string[] = [ "[" ];

      this.contents.next();
      const first = this.contents.tell();
      while (stack.length != 0) {
        if (this.contents.cur().text == "{") {
          stack.push("{");
        } else if (this.contents.cur().text == "}") {
          if (stack.pop() == "[") {
            throw new Error("Expected ']', but saw '}'");
          }
        } else if (this.contents.cur().text == "[") {
          stack.push("[");
        } else if (this.contents.cur().text == "]") {
          if (stack.pop() == "}") {
            throw new Error("Expected '}', but saw ']'");
          }
        }
        this.contents.next();
      }
      const firstAfter = this.contents.tell() - 1;

      value = new JSONX(this.contents.child(first, firstAfter),
                        this);
    } else {
      // Literal
      value = this.get(this.contents.cur().text);
      if (value == undefined) {
        value = new BlobInstance();
        value.set(BlobManager.encoder.encode(
            this.contents.cur().text));
      }
      this.contents.next();
    }

    // Math, lambdas and calls thereof can be here
    let keepLooking = true;
    while (keepLooking) {
      keepLooking = false;
      if (this.contents.cur().text == "(") {
        // Lambda call
        if (!(value instanceof JSONXLambdaBody)) {
          console.log(value.stringify());
          throw new Error("Cannot call non-lambda");
        }

        // Advance past open paren
        this.contents.next();

        // Parse arg object
        let arg = this.parseObject();

        // Advance past close paren
        if (this.contents.cur().text != ")") {
          throw new Error(
              "Missing lambda call closing parenthesis");
        }
        this.contents.next();

        // Replace w/ call
        value = value.call(this, arg);
        keepLooking = true;
      } else if (this.contents.cur().text == "=>") {
        // Lambda definition
        // Value is retroactively the argument name
        this.contents.next();
        value = new JSONXLambdaBody(value.stringify(),
                                    this.parseObject());
        keepLooking = true;
      } else if (this.contents.peek(1).type == "MATH") {
        throw new Error('Math is unimplemented');
        keepLooking = true;
      } else if (this.contents.cur().text == ".") {
        // Path to be resolved
        this.contents.next();
        let name = this.contents.cur().text;
        this.contents.next();
        value = value.get(name);
        keepLooking = true;
      }
    }

    return value;
  }

  //////////////////////////////////////////////////////////////

  ///
  constructor(contents: Pos = new Pos([], 0), parent?: JSONX,
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

  ///
  private ensureResolved() {
    if (!this.isResolved) {
      // Resolve
      this.isResolved = true;
      while (!this.contents.done()) {
        this.parseEntry();
      }
      delete this.contents;
    }
  }

  ///
  get(name: string|number): JSONXVarType|undefined {
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
  add(value: JSONXVarType, name?: string,
      weight: number = 0): JSONXVarType {
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
                const pathStr =
                    (arg as BlobInstance).getString()!;
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
                let contents = arg as BlobInstance;
                const path = contents.getString();
                contents.set(readFileSync(
                    path.substring(1, path.length - 1)));
                return contents;
              }), 'rawf');

/// Localize some object
JSONX.env.add(
    new JSONXLambdaBody('path_or_jsonx', (context, arg) => {
      if (!(context instanceof JSONX)) {
        throw new Error("'include' must be called within an " +
                        "array or scope");
      } else if (arg instanceof JSONXLambdaBody) {
        throw new Error(
            "Cannot use lambda body as argument to 'include'");
      } else if (arg instanceof BlobInstance) {
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
        let toReturn = new BlobInstance();
        toReturn.set(BlobManager.encoder.encode('true'));
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
        let toAdd = new BlobInstance();
        toAdd.set(
            BlobManager.encoder.encode(Math[name].toString()));
        math.add(toAdd, name);
      } else if (name == "max" || name == "min") {
        // Array-input functions
        math.add(new JSONXLambdaBody('arg', (_, arg) => {
                   let input: number[] = [];
                   for (let i = 0; i < (arg as JSONX).length;
                        ++i) {
                     input.push(Number.parseInt(
                         ((arg as JSONX).get(i) as BlobInstance)
                             .getString()));
                   }
                   let out = new BlobInstance();
                   out.set(BlobManager.encoder.encode(
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
                  ((arg as JSONX).get("base") as BlobInstance)
                      .getString());
              const exp = Number.parseInt(
                  ((arg as JSONX).get("exp") as BlobInstance)
                      .getString());
              let out = new BlobInstance();
              out.set(BlobManager.encoder.encode(
                  ((Math[name] as any)(base, exp) as Number)
                      .toString()));
              return out;
            }), name);
      } else {
        // Single-argument functions
        math.add(new JSONXLambdaBody('arg', (_, arg) => {
                   const x = Number.parseInt(
                       (arg as BlobInstance).getString());
                   let out = new BlobInstance();
                   out.set(BlobManager.encoder.encode(
                       ((Math[name] as any)(x) as Number)
                           .toString()));
                   return out;
                 }), name);
      }
    });

export {BlobManager, BlobInstance};
