/**
 * @brief Rewrite of the parser to not evaluate things for as
 * long as possible
 */

import {readFileSync} from "fs";

import {BlobInstance, BlobManager} from "./blob_manager";
import {Pos, tokenize} from "./lexer";

type JSONXVarType = JSONX|BlobInstance|JSONXLambdaBody;

///
class Entry {
  value: JSONXVarType;
  weight: number = 0;
}

///
class NamedEntry extends Entry {
  name: string;
}

/// "statically typed language" my foot
function isNumber(data: unknown): data is number {
  return typeof data === 'number';
}

/// A lambda body which can be evaluated
class JSONXLambdaBody {
  argName: String;
  body: JSONXVarType|((thisJSONX: JSONXVarType,
                       arg: JSONXVarType) => JSONXVarType);
  immediate: boolean;

  constructor(args: String,
              body: JSONXVarType|
              ((thisJSONX: JSONXVarType,
                arg: JSONXVarType) => JSONXVarType),
              immediate: boolean = false) {
    this.argName = args;
    this.body = body;
    this.immediate = immediate;
  }

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

///
export class JSONX {
  ///
  static env = new JSONX();

  static loads(text: string, maxMs?: number,
               maxBytesDA?: number, filepath?: string): JSONX {
    // Set max bytes
    BlobManager.maxBytes = maxBytesDA;

    // If a max time was given, start a timer
    let timeoutID: NodeJS.Timeout;
    if (maxMs != undefined) {
      timeoutID = setTimeout(
          () => {
            throw Error(`Exceeded loadsJSONX time limit of ${
                maxMs} ms`);
          },
      );
    }

    // Lex
    const tokens = tokenize(text, filepath);

    // Parse
    if (tokens.length < 2) {
      return undefined;
    }

    let pos = new Pos(tokens);
    let parsed = new JSONX(pos);

    // If we have a timer running, cancel it
    if (maxMs != undefined) {
      clearTimeout(timeoutID);
    }
    return parsed;
  }

  static loadf(filepath: string, maxMs: number = 60_000,
               maxBytesDA: number = 128_000): JSONX {
    // Load file contents
    const text = readFileSync(filepath).toString();
    return JSONX.loads(text, maxMs, maxBytesDA, filepath);
  }

  //////////////////////////////////////////////////////////////

  ///
  private parent?: JSONX;

  ///
  private contents: Pos;

  ///
  private members = new Map<string, Entry>();

  ///
  private isResolved: boolean = false;

  //////////////////////////////////////////////////////////////

  /// Advances the number until it points to the last token of
  /// an object, logging the object in this.contents. Returns
  /// the new index
  private parseObject(): void {
    // Parse name if provided (note: weight must go in here)
    let name: string = this.members.size.toString();
    if (this.contents.peek().text == ":") {
      name = this.contents.cur().text;
      this.contents.next(2);
    }

    // Parse weight: Currently unused
    let weight = 0;

    // Parse value
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
      const first_after = this.contents.tell();

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
      const first_after = this.contents.tell();

      value = new JSONX(this.contents.child(first, first_after),
                        this);
    } else {
      // Path to be resolved
      let path: string[] = [ this.contents.cur().text ];
      while (this.contents.peek(1).text == ".") {
        this.contents.next(2);
        path.push(this.contents.cur().text);
      }

      // Resolve
      value = this;
      for (const tok of path) {
        if (!(value instanceof JSONX)) {
          throw new Error(
              "Literals and lambdas do not have members");
        }

        value = value.get(tok);
      }
    }

    // Push key-weight-value
    if (!this.members.has(name) ||
        this.members.get(name).weight < weight) {
      this.members.set(name, {value : value, weight : weight});
    }
  }

  //////////////////////////////////////////////////////////////

  ///
  constructor(contents: Pos = new Pos([], 0), parent?: JSONX) {
    this.parent = parent;
    this.contents = contents;
  }

  /// Get the number of members
  get length(): number {
    return this.members.size;
  }

  /// Get the keys of members
  get variables(): NamedEntry[] {
    this.ensureResolved();
    let out: NamedEntry[] = [];
    this.members.forEach((value, key) => {
      let toAdd = new NamedEntry();
      toAdd.name = key;
      toAdd.value = value.value;
      toAdd.weight = value.weight;
      out.push(toAdd);
    });
    return out;
  }

  ///
  private ensureResolved() {
    if (!this.isResolved) {
      // Resolve
      while (!this.contents.done()) {
        this.parseObject();
      }
      this.isResolved = true;
      delete this.contents;
    }
  }

  ///
  get(name: string|number): JSONXVarType {
    name = name.toString();

    if (name == "this") {
      return this;
    } else if (name == "parent") {
      return this.parent;
    } else if (name == "global") {
      if (this.parent == null) {
        return this;
      } else {
        return this.parent.get(name);
      }
    } else if (name == "env") {
      return JSONX.env;
    }

    this.ensureResolved();

    if (this.members.has(name)) {
      return this.members.get(name).value;
    } else {
      let out = new BlobInstance();
      out.set(BlobManager.encoder.encode(name));
      return out;
    }
  }

  add(value: JSONXVarType, name?: string,
      weight: number = 0): JSONXVarType {
    this.ensureResolved();
    if (name == null) {
      name = this.members.size.toString();
    }
    if (!this.members.has(name) ||
        this.members.get(name).weight < weight) {
      this.members.set(name, {value : value, weight : weight});
    }
    return this.members.get(name).value;
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
    }, true), 'include');

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
