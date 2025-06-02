/**
 * @file
 * @brief Parses from a token stream to a resolvable JSONX
 *
 * NOTE: The parser should always be lazy. Inclusions should
 * have weights
 */

import {readFileSync} from "fs";

import {BlobInstance, BlobManager} from "./blob_manager";
import {Pos, Token, tokenize} from "./lexer";

/// "statically typed language" my foot
function isString(data: unknown): data is string {
  return typeof data === 'string';
}
function isNumber(data: unknown): data is number {
  return typeof data === 'number';
}
function isInteger(data: string): boolean {
  const intOrNaN = +data;
  if (isNaN(intOrNaN)) {
    // Not convertable to a number at all
    return false;
  } else {
    return Number.isInteger(intOrNaN);
  }
}

/// A lambda body which can be evaluated
class JSONXLambdaBody {
  argName: String;
  body: JSONXVarType|((thisJSONX: JSONXVarType,
                       arg: JSONXVarType) => JSONXVarType);
  immediate: boolean;

  constructor(args: String,
              body: JSONXVarType|
              ((thisJSONX: JSONX|BlobInstance|JSONXLambdaBody,
                arg: JSONX|BlobInstance|
                JSONXLambdaBody) => JSONXVarType),
              immediate: boolean = false) {
    this.argName = args;
    this.body = body;
    this.immediate = immediate;
  }

  callResolve(thisJSONX: JSONXVarType,
              args: JSONXVarType): JSONXVarType {
    if (this.body instanceof Function) {
      // External call
      return this.body(thisJSONX, args);
    } else {
      // Internal call
      throw new Error("Lambda replacement is unimplemented");
    }
  }

  call(thisJSONX: JSONXVarType,
       args: JSONXVarType): JSONXVarType {
    if (this.immediate) {
      return this.callResolve(thisJSONX, args);
    } else {
      return new JSONXUnresolvedLambdaSubstitution(thisJSONX,
                                                   args, this);
    }
  }
}

/// Resolves to a value, but not until actually queried
class JSONXUnresolvedLambdaSubstitution {
  thisJSONX: JSONXVarType;
  args: JSONXVarType;
  toCall: JSONXLambdaBody;

  constructor(thisJSONX: JSONXVarType, args: JSONXVarType,
              toCall: JSONXLambdaBody) {
    this.thisJSONX = thisJSONX;
    this.args = args;
    this.toCall = toCall;
  }

  resolve(): JSONXVarType {
    // Ensure we are talking about a resolved object
    while (this.thisJSONX instanceof
           JSONXUnresolvedLambdaSubstitution) {
      this.thisJSONX = this.thisJSONX.resolve();
    }

    // Ensure our argument is resolved
    while (this.args instanceof
           JSONXUnresolvedLambdaSubstitution) {
      this.args = this.args.resolve();
    }

    const out =
        this.toCall.callResolve(this.thisJSONX, this.args);
    return out;
  }
}

/// The type of a variable
type JSONXVarType = JSONX|BlobInstance|JSONXLambdaBody|
    JSONXUnresolvedLambdaSubstitution;

/// Can represent a variable, scope, or whatever else.
class JSONXVariableNode {
  name?: String;
  value: JSONXVarType|(Number|String)[];
  weight?: number = 0;
}

/// A nestable, queryable, parsed JSONX object
class JSONX {
  /// Static environment variables, if needed
  static env: JSONX = new JSONX();

  /// If present, the superscope. If not, this is a global
  /// scope. Global scopes are allowed to have a BlobManager.
  private __parent?: JSONX;

  /// The variables in this scope
  variables: JSONXVariableNode[];

  /// Create a root node
  constructor(parent?: JSONX) {
    this.__parent = parent;
    this.variables = [];
  }

  /// Append a new child to the tree
  insert(key?: String, value?: JSONXVarType|(Number|String)[],
         weight?: number): JSONXVariableNode {
    if (value instanceof JSONX) {
      value.__parent = this;
    }
    this.variables.push(
        {name : key, value : value, weight : weight});
    return this.variables[this.variables.length - 1];
  }

  /// Attempt to find the given variable/index in this scope
  get(key: String|Number): JSONXVarType|undefined {
    function internalGet(whom: JSONX) {
      if (isString(key)) {
        if (key == "this") {
          return whom;
        } else if (key == "parent") {
          return whom.__parent;
        } else if (key == "env") {
          return JSONX.env;
        } else if (key == "global") {
          let cur = whom;
          while (cur.__parent != undefined) {
            cur = cur.__parent;
          }
          return cur;
        } else {
          let out: JSONXVariableNode[] = [];
          for (const variable of whom.variables) {
            if (variable.name != undefined &&
                variable.name == key) {
              out.push(variable);
            }
          }
          if (out.length == 0) {
            return undefined;
          }

          // Return the one with the highest weight
          out.sort((a, b) => b.weight - a.weight);
          if (out.length > 1 &&
              out[0].weight == out[1].weight) {
            throw new Error(`Multiple values for '${
                out[0].name}' have weight '${out[0].weight}'`);
          }

          if (!Array.isArray(out[0].value)) {
            return out[0].value;
          } else {
            let cur: JSONXVarType|undefined = whom;
            for (const tok of out[0].value) {
              if (!(cur instanceof JSONX)) {
                return undefined;
              }
              cur = cur.get(tok);
            }
            return cur;
          }
        }
      } else if (isNumber(key) &&
                 key as number < whom.variables.length) {
        // Index
        // NOTE: Indexed items cannot use weights!
        const toReturn = whom.variables[key as number].value;
        if (!Array.isArray(toReturn)) {
          return toReturn;
        } else {
          let cur: JSONXVarType|undefined = whom;
          for (const tok of toReturn) {
            if (!(cur instanceof JSONX)) {
              return undefined;
            }
            cur = cur.get(tok);
          }
          return cur;
        }
      }

      // Invalid index
      return undefined;
    }

    // Fully resolve lambda calls
    let out = internalGet(this);
    if (out instanceof JSONXUnresolvedLambdaSubstitution) {
      return out.resolve();
    }
    return out;
  }

  get length(): number {
    return this.variables.length;
  }

  get isArray(): boolean {
    return this.length == 0 ||
           this.variables.at(0).name == undefined;
  }

  /// Write (a rough approximation of) the original JSONX string
  stringify(tabbing: string = "", tab: string = "  "): string {
    function stringifyValue(
        value: JSONXVarType|(String | Number)[],
        tabbing: string, tab: string): string {
      if (Array.isArray(value)) {
        return value.join('.');
      } else if (value instanceof JSONX) {
        return value.stringify(tabbing, tab);
      } else if (value instanceof BlobInstance) {
        return value.getString();
      } else if (value instanceof JSONXLambdaBody) {
        let out = value.argName + " => ";
        if (value.body instanceof Function) {
          out += "...";
        } else {
          out += stringifyValue(value.body, tabbing + tab + tab,
                                tab);
        }
        return out;
      } else if (value instanceof
                 JSONXUnresolvedLambdaSubstitution) {
        out += stringifyValue(value.toCall, tabbing, tab);
        out += "(";
        out += out +=
            stringifyValue(value.args, tabbing + tab, tab);
      } else {
        console.log(
            '/* Unexpected variable type:', typeof value, '*/');
      }
    }

    let out = "";

    // If no variables, just do a unit scope
    if (this.variables.length == 0) {
      out = "{}";
    }

    // If the variables are unnamed, this is an array
    else if (this.isArray) {
      out = "[\n";
      let first = true;
      for (let variable of this.variables) {
        if (first) {
          first = false;
        } else {
          out += ",\n";
        }
        out += tabbing + tab +
               stringifyValue(variable.value,
                              tabbing + tab + tab, tab);
      }
      out += "\n" + tabbing + "]";
    }

    // Otherwise, this is a scope
    else {
      out = "{\n";
      let first = true;
      for (let variable of this.variables) {
        if (first) {
          first = false;
        } else {
          out += ",\n";
        }
        out += tabbing + tab + variable.name;
        // weight goes here
        if (variable.weight != undefined) {
          if (variable.weight >= 0) {
            out += '!'.repeat(variable.weight);
          } else {
            out += '?'.repeat(-variable.weight);
          }
        }
        out += ": " + stringifyValue(variable.value,
                                     tabbing + tab + tab, tab);
      }
      out += "\n" + tabbing + "}";
    }

    return out;
  }

  /**
   * @brief Load a JSONX-formatted string to a JS object
   * @param text The JSONX text to load
   * @param maxMs The max number of ms to give the process
   * @param maxBytesDA The max number of bytes dynamically
   *     allocatable
   * @returns The JS object represented by the JSONX text
   */
  static loads(text: string, maxMs: number = 60_000,
               maxBytesDA: number = 128_000, filepath?: string):
      JSONXVarType|(String|Number)[]|undefined {
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
    const parsed = parseJSONX(tokens);

    // If we have a timer running, cancel it
    if (maxMs != undefined) {
      clearTimeout(timeoutID);
    }
    return parsed;
  }

  /**
   * @brief Load a JSONX-formatted file to a JS object
   * @param text The filepath of a JSONX file to load
   * @param maxMs The max number of ms to give the process
   * @param maxBytesDA The max number of bytes dynamically
   *     allocatable
   * @returns The JS object represented by the JSONX text
   */
  static loadf(filepath: string, maxMs: number = 60_000,
               maxBytesDA: number = 128_000): JSONXVarType|
      (String|Number)[]|undefined {
    // Load file contents
    const text = readFileSync(filepath).toString();
    return JSONX.loads(text, maxMs, maxBytesDA, filepath);
  }
}

/// Turns a token stream (manages by the Pos arg) and turns it
/// into a parse tree whose parent is context.
function parseScope(pos: Pos, context?: JSONX): JSONX {
  let out: JSONX = new JSONX(context);
  pos.expect('{');
  while (pos.peek().text != '}') {
    let identifier = pos.next().text;

    if (pos.peek().text == ".") {
      throw new Error(
          "LHS identifiers must contain exactly one token");
    } else if (isInteger(identifier)) {
      throw new Error("LHS identifiers must not be indices");
    } else {
      if (identifier.startsWith('"') ||
          identifier.startsWith('\'') ||
          identifier.startsWith('`')) {
        identifier =
            identifier.substring(1, identifier.length - 1);
      }
    }
    const weight = parseWeight(pos);
    pos.expect(':');
    const rhs = parseExpression(pos, out);
    out.insert(identifier, rhs, weight);
    while (pos.peek().text == ',') {
      pos.next();
    }
  }
  pos.expect('}');
  return out;
}

function parseArray(pos: Pos, context?: JSONX): JSONX {
  let out = new JSONX(context);
  pos.expect('[');
  while (pos.peek().text != ']') {
    let rhs = parseExpression(pos, out);
    out.insert(undefined, rhs);
    while (pos.peek().text == ',') {
      pos.next();
    }
  }
  pos.expect(']');
  return out;
}

/// A reference to an existing value: e.g. "a.b.0.d.e" ->
/// ["a", "b", 0, "d", "e"]
function parseIdentifierRHS(pos: Pos): (String|Number)[] {
  let out: (String|Number)[] = [];
  const tok = pos.next();
  if (tok.type == "ID") {
    out.push(tok.text as String);
  } else {
    throw new Error(
        `'${tok.text}' is not a valid RHS first identifier.`);
  }
  while (pos.peek().text == ".") {
    pos.next();
    const tok = pos.next();
    if (tok.type == "ID") {
      if (isInteger(tok.text)) {
        out.push(Number.parseInt(tok.text) as Number);
      } else {
        out.push(tok.text as String);
      }
    } else if (tok.text.startsWith('"') ||
               tok.text.startsWith('\'') ||
               tok.text.startsWith('`')) {
      out.push(tok.text.substring(1, tok.text.length - 1) as
               String);
    } else {
      throw new Error(
          `'${tok.text}' is not a valid identifier.`);
    }
  }
  return out;
}

/// The weight on a particular element of an object
function parseWeight(pos: Pos): number {
  let out = 0;
  if (pos.peek().text == '?') {
    while (pos.peek().text == '?') {
      --out;
      pos.next();
    }
  } else if (pos.peek().text == '!') {
    while (pos.peek().text == '!') {
      ++out;
      pos.next();
    }
  }
  return out;
}

/// The RHS of a statement
function parseExpression(pos: Pos, context?: JSONX):
    JSONXVarType|(Number | String)[] {
  const tok = pos.peek();
  let obj: JSONXVarType|undefined|(Number | String)[] =
      undefined;
  if (tok.text == '[') {
    obj = parseArray(pos, context);
  } else if (tok.text == '{') {
    obj = parseScope(pos, context);
  } else if (tok.type == "ID" && !isInteger(tok.text)) {
    obj = parseIdentifierRHS(pos);
  } else if (tok.type == "LIT" || tok.type == "ID") {
    obj = new BlobInstance();
    obj.set(BlobManager.encoder.encode(tok.text));
    pos.next();
  } else {
    throw new Error(`Failed to parse token '${tok.text}'`);
  }

  // Resolve suffix expressions (e.g. lambdas)
  let didThing = true;
  while (didThing) {
    didThing = false;
    const next = pos.peek();
    if (next.type == "EOF") {
      break;
    } else if (next.text == "=>") {
      if (!Array.isArray(obj) || obj.length != 1 ||
          !isString(obj[0])) {
        throw new Error(
            "Lambda argument must be a single LHS identifier");
      }
      pos.next();
      let body = parseExpression(pos, context);
      if (Array.isArray(body)) {
        let cur: JSONXVarType|undefined = context;
        for (const tok of body) {
          if (!(cur instanceof JSONX)) {
            return undefined;
          }
          cur = cur.get(tok);
        }
        body = cur;
      }
      let lambda = new JSONXLambdaBody(obj[0], body);
      obj = lambda;
      didThing = true;
    } else if (next.text == "(") {
      // Lambda reduction
      if (Array.isArray(obj)) {
        let cur: JSONXVarType|undefined = context;
        for (const tok of obj) {
          if (!(cur instanceof JSONX)) {
            return undefined;
          }
          cur = cur.get(tok);
        }
        obj = cur;
      }

      // Ensure we are talking about a lambda
      if (!(obj instanceof JSONXLambdaBody)) {
        throw new Error("Cannot call non-lambda");
      }

      // Parse call
      pos.expect("(");
      let args = parseExpression(pos, context);
      pos.expect(")");

      if (Array.isArray(args)) {
        let cur: JSONXVarType|undefined = context;
        for (const tok of args) {
          if (!(cur instanceof JSONX)) {
            return undefined;
          }
          cur = cur.get(tok);
        }
        args = cur;
      }

      // Yield an object which, when resolved, will be the
      // result of the call
      obj = obj.call(context, args);
      didThing = true;
    } else if (pos.peek().type == "MATH") {
      throw new Error("Math is unimplemented");
    }
  }
  return obj;
}

////////////////////////////////////////////////////////////////

/// Parse from token stream
function parseJSONX(lexed: Token[]): JSONXVarType|
    (String | Number)[]|undefined {
  if (lexed.length < 2) {
    return undefined;
  }

  let pos = new Pos(lexed);
  let toReturn = parseExpression(pos);
  pos.expect('EOF');

  return toReturn;
}

/// Load a file as a scope
JSONX.env.insert(
    'loadf', new JSONXLambdaBody('path', (_, arg) => {
      const pathStr = (arg as BlobInstance).getString()!;
      const out =
          JSONX.loadf(pathStr.substring(1, pathStr.length - 1));
      if (!Array.isArray(out)) {
        return out;
      } else {
        return undefined;
      }
    }));

/// Load a file as a raw blob
JSONX.env.insert(
    'rawf', new JSONXLambdaBody('path', (context, arg) => {
      let contents = arg as BlobInstance;
      const path = contents.getString();
      contents.set(
          readFileSync(path.substring(1, path.length - 1)));
      return contents;
    }));

/// Localize
JSONX.env.insert(
    'include',
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
            .callResolve(
                context,
                (JSONX.env.get("loadf") as JSONXLambdaBody)
                    .callResolve(context, arg));
      } else {
        // JSONX to localize
        for (let i = 0; i < arg.length; ++i) {
          const variable = arg.variables.at(i);
          context.insert(variable.name, variable.value,
                         variable.weight);
        }
        let toReturn = new BlobInstance();
        toReturn.set(BlobManager.encoder.encode('true'));
        return toReturn;
      }
    }, true));

let math = JSONX.env.insert("math", new JSONX()).value as JSONX;
["E", "LN10", "LN2", "LOG2E", "LOG10E", "PI", "SQRT1_2",
 "SQRT2", "abs", "acos", "asin", "atan", "ceil", "cos", "exp",
 "floor", "log", "max", "min", "pow", "round", "sin", "sqrt",
 "tan"]
    .forEach((value) => {
      if (isNumber(Math[value] as any)) {
        // Raw numbers
        let toAdd = new BlobInstance();
        toAdd.set(
            BlobManager.encoder.encode(Math[value].toString()));
        math.insert(value, toAdd);
      } else if (value == "max" || value == "min") {
        // Array-input functions
        math.insert(
            value,
            new JSONXLambdaBody('arg', (context, arg) => {
              let input: number[] = [];
              for (let i = 0; i < (arg as JSONX).length; ++i) {
                input.push(Number.parseInt(
                    ((arg as JSONX).get(i) as BlobInstance)
                        .getString()));
              }
              let out = new BlobInstance();
              out.set(BlobManager.encoder.encode(
                  ((Math[value] as any)(input) as Number)
                      .toString()));
              return out;
            }));
      } else if (value == "pow") {
        // Two-argument function
        // env.math.pow({base: 123, exp: 123})
        math.insert(
            value,
            new JSONXLambdaBody('arg', (context, arg) => {
              const base = Number.parseInt(
                  ((arg as JSONX).get("base") as BlobInstance)
                      .getString());
              const exp = Number.parseInt(
                  ((arg as JSONX).get("exp") as BlobInstance)
                      .getString());
              let out = new BlobInstance();
              out.set(BlobManager.encoder.encode(
                  ((Math[value] as any)(base, exp) as Number)
                      .toString()));
              return out;
            }));
      } else {
        // Single-argument functions
        math.insert(value, new JSONXLambdaBody('arg', (context,
                                                       arg) => {
                      const x = Number.parseInt(
                          (arg as BlobInstance).getString());
                      let out = new BlobInstance();
                      out.set(BlobManager.encoder.encode(
                          ((Math[value] as any)(x) as Number)
                              .toString()));
                      return out;
                    }));
      }
    });

export {
  BlobInstance,
  BlobManager,
  JSONX,
};
