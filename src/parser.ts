/**
 * @file
 * @brief Parses from a token stream to a resolvable JSONX
 *
 * NOTE: The parser should always be lazy. Inclusions should
 * have weights
 */

import {BlobInstance, BlobManager} from "./blob_manager";
import {loadfJSONX} from "./jsonx";
import {ParseError, Pos, Token} from "./lexer";

/// "statically typed language" my foot
function isString(data: unknown): data is string {
  return typeof data === 'string';
}
function isNumber(data: unknown): data is number {
  return typeof data === 'number';
}

///
type JSONXVarType = JSONX|Token|BlobInstance|JSONXLambdaBody;

///
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

  call(thisJSONX: JSONX, args: JSONXVarType): JSONXVarType {
    if (this.body instanceof Function) {
      // External call
      return this.body(thisJSONX, args);
    } else {
      // Internal call
      throw new Error("Lambda replacement is unimplemented");
    }
  }
}

/// Can represent a variable, scope, or whatever else.
class JSONXVariableNode {
  name?: String;
  value: JSONXVarType|(Number|String)[];
  weight?: number = 0;
}

///
class JSONX {
  /// Static environment variables, if needed
  static env: JSONX = new JSONX();

  /// If present, the superscope. If not, this is a global
  /// scope. Global scopes are allowed to have a BlobManager.
  private __parent?: JSONX;

  /// Present only if this is a global scope. Instantiated on
  /// first request.
  private __blobManager?: BlobManager;

  /// The variables in this scope
  private __variables: JSONXVariableNode[];

  /// Create a root node
  constructor(parent?: JSONX) {
    this.__parent = parent;
    this.__blobManager = undefined;
    this.__variables = [];
  }

  /// Finds the nearest blob manager and returns it
  get blobManager(): BlobManager {
    if (this.__parent != undefined) {
      return this.__parent.blobManager;
    } else if (this.__blobManager == undefined) {
      this.__blobManager = new BlobManager();
    }
    return this.__blobManager;
  }

  /// Append a new child to the tree
  insert(key?: String|String[],
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
        let newScope =
            this.insert(localName, new JSONX(this)).value as
            JSONX;
        return newScope.insert(key, value, weight);
      }
    } else {
      if (value instanceof JSONX) {
        if (value.__blobManager != undefined) {
          throw new Error("Invalid blob manager hierarchy");
        }
        value.__parent = this;
      }
      this.__variables.push(
          {name : key, value : value, weight : weight});
      return this.__variables[this.__variables.length - 1];
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
      for (const variable of this.__variables) {
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
               key as number < this.__variables.length) {
      // Index
      // NOTE: Indexed items cannot use weights!
      const toReturn = this.__variables[key as number].value;
      if (!Array.isArray(toReturn)) {
        return toReturn;
      } else {
        return this.get(toReturn);
      }
    }

    // Invalid index
    return undefined;
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
      } else if (value instanceof Token) {
        return value.text;
      } else if (value instanceof BlobInstance) {
        throw new Error(
            "BlobInstance stringify is unimplemented");
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
    if (this.__variables.length == 0) {
      out = "{}";
    }

    // If the variables are unnamed, this is an array
    else if (this.__variables.at(0).name == undefined) {
      out = "[\n";
      let first = true;
      for (let variable of this.__variables) {
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
      for (let variable of this.__variables) {
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
      out.push(tok.text as String);
    } else if (tok.type == "NUM") {
      out.push(Number.parseInt(tok.text) as Number);
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
  } else if (tok.type == "NUM") {
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

  else if (tok.type == "ID") {
    // Identifier CHAIN referring to an existing value
    obj = parseIdentifierRHS(pos);
  } else if (tok.type == "LIT" || tok.type == "NUM") {
    // Single-token literal
    obj = tok;

    // Advance to first tok after literal
    pos.next();
  }

  let didThing = true;
  while (didThing) {
    didThing = false;
    const next = pos.peek();
    if (next.text == "=>") {
      // Lambda def
      if (!Array.isArray(obj) || obj.length != 1 ||
          !isString(obj[0])) {
        console.log(obj);
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
        console.log(obj);
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
      // Binary math ops
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
  if (lexed.length == 0) {
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

JSONX.env.insert(
    'loadf', new JSONXLambdaBody('path', (context, arg) => {
      if (!(arg instanceof Token) || arg.type != "LIT") {
        throw new Error(
            "Cannot call 'env.loadf' without a string-token " +
            "path");
      }
      let path = arg.text.substring(1, arg.text.length - 1);
      return loadfJSONX(path, undefined, undefined,
                        context as JSONX);
    }));

export {parseJSONX, JSONXVarType, JSONX};
