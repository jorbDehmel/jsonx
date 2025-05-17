/**
 * @file
 * @brief Parses from a token stream to a resolvable JSONX
 *
 * NOTE: The parser should always be lazy. Inclusions should
 * have weights
 */

import assert = require("assert");
import {BlobInstance, BlobManager} from "./blob_manager";
import {ParseError, Pos, Token} from "./lexer";

///
class JSONXLambdaBody {}

///
type JSONXVarType =
    JSONX|Token|Token[]|BlobInstance|JSONXLambdaBody;

/// Can represent a variable, scope, or whatever else.
class JSONXVariableNode {
  name?: string;
  value: JSONXVarType;
  weight?: number;
}

///
class JSONX {
  /// If present, the superscope. If not, this is a global
  /// scope. Global scopes are allowed to have a BlobManager.
  private __parent?: JSONX;

  /// Present only if this is a global scope. Instantiated on
  /// first request.
  private __blobManager?: BlobManager;

  /// The variables in this scope
  private __variables: JSONXVariableNode[];

  /// Create a root node
  constructor() {
    this.__parent = undefined;
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
  insert(key?: Token|Token[], value?: JSONXVarType,
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
            this.insert(localName, new JSONX()).value as JSONX;
        return newScope.insert(key, value, weight);
      }
    } else {
      if (value instanceof JSONX) {
        assert(value.__blobManager == undefined);
        value.__parent = this;
      }
      this.__variables.push({
        name : key?.text ?? undefined,
        value : value,
        weight : weight
      });
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
    } else if (key instanceof String) {
      // Variable name
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
        // if a < b, return negative
        // if a = b, return 0
        // if a > b, return positive
        return a.weight - b.weight;
      })

      return out[0];
    } else if (key instanceof Number &&
               key as number < this.__variables.length) {
      // Index
      // NOTE: Indexed items cannot use weights!
      return this.__variables[key as number];
    }

    // Invalid index
    return undefined;
  }

  /// Calls a lambda function
  call(): JSONXVarType {
  }
}

/// Turns a token stream (manages by the Pos arg) and turns it
/// into a parse tree
function parseScope(pos: Pos): JSONX {
  let out: JSONX = new JSONX();
  pos.expect('{');
  while (pos.peek().text != '}') {
    // Identifier (possibly compound, e.g. a.b.c, but never
    // indicized)
    let identifier = parseIdentifierLHS(pos);
    let weight = parseWeight(pos);

    // Colon
    pos.expect(':');

    // RHS
    let rhs = parseExpression(pos);

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

function parseArray(pos: Pos): JSONX {
  let out = new JSONX();
  pos.expect('[');

  while (pos.peek().text != ']') {
    // RHS
    let rhs = parseExpression(pos);

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
function parseIdentifier(pos: Pos): Token[] {
  let out: Token[] = [];

  // One name is required
  out.push(pos.next());

  // But any number more can follow that
  while (pos.peek().text == ".") {
    pos.next();

    const tok = pos.next();
    if (tok.type != "ID" && tok.type != "NUM") {
      throw new Error(
          `'${tok.text}' is not a valid identifier.`);
    }

    out.push(tok);
  }

  return out;
}

/// The same as `parseIdentifier`, but only allows LHS-viable
/// paths (no indices)
function parseIdentifierLHS(pos: Pos): Token[] {
  let identifier = parseIdentifier(pos);
  let cleanedIdentifier: Token[] = [];
  identifier.forEach((item) => {
    if (item.type == "ID") {
      cleanedIdentifier.push(item);
    } else {
      throw new Error("Cannot use indices in LHS identifier");
    }
  });
  return cleanedIdentifier;
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
function parseExpression(pos: Pos): JSONXVarType {
  const tok = pos.peek();
  let obj: JSONXVarType = undefined;
  if (tok.text == '[') {
    // Array
    obj = parseArray(pos);
  } else if (tok.text == '{') {
    // Scope
    obj = parseScope(pos);
  }

  else if (tok.type == "ID") {
    // Identifier CHAIN referring to an existing value
    obj = parseIdentifier(pos);
  } else if (tok.type == "LIT" || tok.type == "NUM") {
    // Single-token literal
    obj = tok;

    // Advance to first tok after literal
    pos.next();
  }

  // MATH, LAMBDA DEF, FN CALL STUFF GOES HERE!!!!!!!!!
  if (pos.peek().text == "=>") {
    // Lambda def
  } else if (pos.peek().text == "(") {
    // Lambda call
  }

  return obj;
}

////////////////////////////////////////////////////////////////

/// Parse from token stream
function parseJSONX(lexed: Token[]): JSONXVarType {
  return parseExpression(new Pos(lexed));
}

export {parseJSONX, JSONXVarType, JSONX};
