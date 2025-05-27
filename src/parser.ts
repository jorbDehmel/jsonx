/**
 * @file
 * @brief Parses from a token stream to a resolvable JSONX
 *
 * NOTE: The parser should always be lazy. Inclusions should
 * have weights
 */

import {readFileSync} from "fs";

import {BlobInstance, BlobManager} from "./blob_manager";
import {ParseError, Pos, Token, tokenize} from "./lexer";

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
  args: String;
  body: JSONXVarType|((thisJSONX: JSONXVarType,
                       arg: JSONXVarType) => JSONXVarType);

  constructor(args: String, body: JSONXVarType|
              ((thisJSONX: JSONXVarType,
                arg: JSONXVarType) => JSONXVarType)) {
    this.args = args;
    this.body = body;
  }

  call(thisJSONX: JSONXVarType,
       args: JSONXVarType): JSONXVarType {
    if (this.body instanceof Function) {
      // External call
      return this.body(thisJSONX, args);
    } else {
      // Internal call
      throw new Error("Lambda replacement is unimplemented");
    }
  }
}

/// The type of a variable
type JSONXVarType = JSONX|BlobInstance|JSONXLambdaBody;

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
  insert(key?: String|(String[]),
         value?: JSONXVarType|(Number|String)[],
         weight?: number): JSONXVariableNode {
    if (Array.isArray(key)) {
      if (key.length < 1) {
        throw new ParseError(
            'Cannot add variable with empty compound path!');
      } else if (key.length == 1) {
        return this.insert(key[0], value, weight);
      } else {
        let localName = key[0];
        key.splice(0, 1);
        let newScope: JSONX;
        let found = this.get(localName);
        if (found instanceof JSONX) {
          newScope = found;
        } else {
          newScope =
              this.insert(localName, new JSONX(this)).value as
              JSONX;
        }
        return newScope.insert(key, value, weight);
      }
    } else {
      if (value instanceof JSONX) {
        value.__parent = this;
      }
      this.variables.push(
          {name : key, value : value, weight : weight});
      return this.variables[this.variables.length - 1];
    }
  }

  /// Attempt to find the given variable/index in this scope
  get(key: String|Number|(String|Number)[]): JSONXVarType
      |undefined {
    if (Array.isArray(key)) {
      if (key.length > 0) {
        let child = this.get(key[0]);
        if (key.length == 1) {
          return child;
        } else if (child instanceof JSONX) {
          key.splice(0, 1);
          return child.get(key);
        }
      }
    } else if (isString(key)) {
      // Variable name
      if (key == "this") {
        return this;
      } else if (key == "parent") {
        return this.__parent;
      } else if (key == "env") {
        return JSONX.env;
      } else if (key == "global") {
        if (this.__parent == undefined) {
          return this;
        }
        let cur = this.__parent;
        while (cur.__parent != undefined) {
          cur = cur.__parent;
        }
        return cur;
      }

      let out: JSONXVariableNode[] = [];
      for (const variable of this.variables) {
        if (variable.name != undefined &&
            variable.name == key) {
          out.push(variable);
        }
      }

      if (out.length == 0) {
        return undefined;
      }

      // Return the one with the highest weight
      out.sort((a: JSONXVariableNode, b: JSONXVariableNode) => {
        return b.weight - a.weight;
      })

      if (out.length > 1 && out[0].weight == out[1].weight) {
        throw new Error(`Multiple values for '${
            out[0].name}' have weight '${out[0].weight}'`);
      }

      if (!Array.isArray(out[0].value)) {
        return out[0].value;
      } else {
        return this.get(out[0].value);
      }
    } else if (isNumber(key) &&
               key as number < this.variables.length) {
      // Index
      // NOTE: Indexed items cannot use weights!
      const toReturn = this.variables[key as number].value;
      if (!Array.isArray(toReturn)) {
        return toReturn;
      } else {
        return this.get(toReturn);
      }
    }

    // Invalid index
    return undefined;
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
        let out = value.args + " => ";
        if (value.body instanceof Function) {
          out += "...";
        } else {
          out += stringifyValue(value.body, tabbing + tab + tab,
                                tab);
        }
        return out;
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
               maxBytesDA: number = 128_000, context?: JSONX,
               filepath?: string): JSONXVarType|undefined {
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
    const parsed = parseJSONX(tokens, context);

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
               maxBytesDA: number = 128_000,
               context?: JSONX): JSONXVarType|undefined {
    // Load file contents
    const text = readFileSync(filepath).toString();
    return JSONX.loads(text, maxMs, maxBytesDA, context,
                       filepath);
  }
}

/// Turns a token stream (manages by the Pos arg) and turns it
/// into a parse tree whose parent is context.
function parseScope(pos: Pos, context?: JSONX): JSONX {
  let out: JSONX = new JSONX(context);
  pos.expect('{');
  while (pos.peek().text != '}') {
    // Identifier (possibly compound, e.g. a.b.c, but never
    // indicized)
    let identifier = parseIdentifierLHS(pos);
    let weight = parseWeight(pos);

    // Colon
    pos.expect(':');

    // RHS
    let rhs = parseExpression(pos, out);

    // Add to scope
    out.insert(identifier, rhs, weight);

    // Optional comma
    while (pos.peek().text == ',') {
      pos.next();
    }
  }
  pos.expect('}');

  // Now pointing to first token after
  return out;
}

function parseArray(pos: Pos, context?: JSONX): JSONX {
  let out = new JSONX(context);
  pos.expect('[');

  while (pos.peek().text != ']') {
    // RHS
    let rhs = parseExpression(pos, out);

    // Add to scope
    out.insert(undefined, rhs);

    // Optional comma
    while (pos.peek().text == ',') {
      pos.next();
    }
  }

  pos.expect(']');

  // Now pointing to first token after
  return out;
}

/// A reference to an existing value: e.g. "a.b.0.d.e" ->
/// ["a", "b", 0, "d", "e"]
function parseIdentifierRHS(pos: Pos): (String|Number)[] {
  let out: (String|Number)[] = [];

  // One name is required
  const tok = pos.next();
  if (tok.type == "ID") {
    out.push(tok.text as String);
  } else {
    throw new Error(
        `'${tok.text}' is not a valid RHS first identifier.`);
  }

  // But any number more can follow that, with slightly looser
  // restrictions (indices and strings)
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

/// The same as `parseIdentifier`, but only allows LHS-viable
/// paths (no indices)
function parseIdentifierLHS(pos: Pos): String {
  let tok = pos.next();
  if (pos.peek().text == ".") {
    throw new Error(
        "LHS identifiers must contain exactly one token");
  } else if (isInteger(tok.text)) {
    throw new Error("LHS identifiers must not be indices");
  } else {
    if (tok.text.startsWith('"') || tok.text.startsWith('\'') ||
        tok.text.startsWith('`')) {
      return tok.text.substring(1, tok.text.length - 1) as
             String;
    } else {
      return tok.text as String;
    }
  }
}

function parseWeight(pos: Pos): number {
  let out = 0;
  if (pos.peek().text == '?') {
    while (pos.peek().text == '?') {
      --out;
      pos.next();
      if (pos.peek().text == '!') {
        throw new ParseError('Cannot have both \'?\' and \'!\'',
                             pos.peek());
      }
    }
  } else if (pos.peek().text == '!') {
    while (pos.peek().text == '!') {
      ++out;
      pos.next();
      if (pos.peek().text == '?') {
        throw new ParseError('Cannot have both \'?\' and \'!\'',
                             pos.peek());
      }
    }
  }

  // Now pointing to first token after weighting
  return out;
}

/*
The RHS of a statement

Easy + suffix options
- A lambda / arrow function
- A function call on some expressions

Hard:
- Math
*/
function parseExpression(pos: Pos, context?: JSONX):
    JSONXVarType|(Number | String)[] {
  const tok = pos.peek();
  let obj: JSONXVarType|undefined|(Number | String)[] =
      undefined;
  if (tok.text == '[') {
    // Array
    obj = parseArray(pos, context);
  } else if (tok.text == '{') {
    // Scope
    obj = parseScope(pos, context);
  }

  else if (tok.type == "ID" && !isInteger(tok.text)) {
    // Identifier CHAIN referring to an existing value
    obj = parseIdentifierRHS(pos);
  } else if (tok.type == "LIT" || tok.type == "ID") {
    // Single-token literal (string, bool, or non-index ID)
    obj = new BlobInstance();
    obj.set(BlobManager.encoder.encode(tok.text));

    // Advance to first tok after literal
    pos.next();
  } else {
    throw new Error(`Failed to parse token '${tok.text}'`);
  }

  let didThing = true;
  while (didThing) {
    didThing = false;
    const next = pos.peek();
    if (next.type == "EOF") {
      break;
    } else if (next.text == "=>") {
      // Lambda def
      if (!Array.isArray(obj) || obj.length != 1 ||
          !isString(obj[0])) {
        throw new Error(
            "Lambda argument must be a single LHS identifier");
      }
      pos.next();
      let body = parseExpression(pos, context);
      if (Array.isArray(body)) {
        body = context.get(body);
      }
      let lambda = new JSONXLambdaBody(obj[0], body);
      obj = lambda;
      didThing = true;
    } else if (next.text == "(") {
      // Lambda reduction
      if (Array.isArray(obj)) {
        obj = context.get(obj);
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
        args = context.get(args);
      }

      // Do the call
      obj = obj.call(context, args);
      didThing = true;
    } else if (pos.peek().type == "MATH") {
      // Binary math operations
      throw new Error("Math is unimplemented");
      didThing = true;
    }
  }

  return obj;
}

////////////////////////////////////////////////////////////////

/// Parse from token stream
function parseJSONX(lexed: Token[],
                    context?: JSONX): JSONXVarType|undefined {
  if (lexed.length < 2) {
    return undefined;
  }

  let pos = new Pos(lexed);
  let toReturn = parseExpression(pos, context);
  pos.expect('EOF');

  if (Array.isArray(toReturn)) {
    return context?.get(toReturn);
  }
  return toReturn;
}

/// Load a file as a scope
JSONX.env.insert(
    'loadf', new JSONXLambdaBody('path', (context, arg) => {
      const pathStr = (arg as BlobInstance).getString()!;
      return JSONX.loadf(
          pathStr.substring(1, pathStr.length - 1), undefined,
          undefined, context as JSONX);
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
            .call(context,
                  (JSONX.env.get("loadf") as JSONXLambdaBody)
                      .call(context, arg));
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
    }));

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
        JSONX.env.insert([ "math", value ], toAdd);
      } else if (value == "max" || value == "min") {
        // Array-input functions
        JSONX.env.insert(
            [ "math", value ],
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
        JSONX.env.insert(
            [ "math", value ],
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
        JSONX.env.insert(
            [ "math", value ],
            new JSONXLambdaBody('arg', (context, arg) => {
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

export {JSONXVarType, JSONX};
