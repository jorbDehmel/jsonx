'use strict';

/**
 * @file
 * @brief Parses JSONX from a token stream to a resolvable (but
 * not resolved) object
 */

import {Token} from "./lexer";

/// Thrown when a parse error occurs
class ScopeError {
  msg: string;
  tok?: Token;

  constructor(msg: string, tok?: Token) {
    this.msg = msg;
    this.tok = tok;
  }
}

/// Represents a position in a token stream
class Pos {
  tokens: Token[];
  pos: number;

  constructor(tokens: Token[], pos: number = 0) {
    this.tokens = tokens;
    this.pos = pos;
  }

  /// Consume and return the next token Returns <EOF> if we are
  // already at the end.
  next(): Token {
    let tok = this.peek();
    ++this.pos;
    return tok;
  }

  /// Get the next token, throwing an error if it doesn't match.
  /// This is CONSUMPTIVE.
  expect(text?: string, type?: string) {
    let n = this.next();
    if (text != undefined && n.text != text) {
      throw new ScopeError(
          `Expected token text '${text}', but saw '${n.text}'`,
          n);
    }
    if (type != undefined && n.text != text) {
      throw new ScopeError(
          `Expected token type '${type}', but saw '${n.type}'`,
          n);
    }
  }

  /// Look at the next token non-comsumptively. Returns <EOF> if
  /// we are already at the end.
  peek() {
    if (this.pos >= this.tokens.length) {
      // Out of range!
      return new Token("<EOF>", "EOF");
    } else {
      // Not out of range.
      let tok = this.tokens[this.pos];
      return tok;
    }
  }
}

/// Can represent a variable, scope, or whatever else.
class Node {
  name?: string;
  value?: object;
  weight?: number;
}

/// A scope in a parse tree
class Scope {
  parent?: Scope;
  variables: Node[];

  /// Create a root node
  constructor() {
    this.parent = undefined;
    this.variables = [];
  }

  /// Append a new child to the tree
  newChild(key?: string, value?: object,
           weight?: number): object {
    if (value instanceof Scope) {
      value.parent = this;
    }
    this.variables.push(
        {name : key, value : value, weight : weight});
    return this.variables[this.variables.length - 1];
  }

  /// Allows compound names to be added
  newChildRecursive(identifier: string[], value?: object,
                    weight?: number): object {
    if (identifier.length < 1) {
      throw new ScopeError(
          'Cannot add variable with empty compound path!');
    } else if (identifier.length == 1) {
      return this.newChild(identifier[0], value, weight);
    } else {
      let localName = identifier[0];
      identifier.splice(0, 1);
      let newScope =
          this.newChild(localName, new Scope()) as Scope;
      return newScope.newChildRecursive(identifier, value,
                                        weight);
    }
  }

  /// Attempt to find the given variable/index in this scope
  getChild(key: String|Number): object[] {
    if (key instanceof String) {
      // Variable name
      let out: object[] = [];
      for (const variable of this.variables) {
        if (variable.name != undefined &&
            variable.name == key) {
          out.push(variable);
        }
      }
      return out;
    } else if (key as number < this.variables.length) {
      // Index
      return [ this.variables[key as number] ];
    }

    // Invalid index
    return [];
  }

  /// Use a nested identifier to locate a child. For instance,
  /// "a.b.0" would attempt to find "a", then ask it for "b",
  /// then ask it for "0".
  getChildRecursive(identifier: (String|Number)[]): object
      |undefined {
    if (identifier.length > 0) {
      let child = this.getChild(identifier[0]);
      if (identifier.length == 1) {
        return child;
      } else if (child instanceof Scope) {
        identifier.splice(0, 1);
        return child.getChildRecursive(identifier);
      }
    }
    return undefined;
  }
}

/// Turns a token stream (manages by the Pos arg) and turns it
/// into a parse tree
function parseScope(pos: Pos): Scope {
  let out: Scope = new Scope();
  pos.expect('{');
  while (pos.peek().text != '}') {
    // Identifier (possibly compound, e.g. a.b[4].c)
    let identifier = parseIdentifier(pos);
    let weight = parseWeight(pos);

    // Colon
    pos.expect(':');

    // RHS
    let rhs = parseExpression(pos);

    // Add to scope
    out.newChildRecursive(identifier, rhs, weight);

    // Optional comma
    while (pos.peek().text == ',') {
      pos.next();
    }
  }
  pos.expect('}');
  return out;
}

function parseArray(pos: Pos): Scope {
  let out = new Scope();
  pos.expect('[');

  // RHS
  let rhs = parseExpression(pos);

  // Add to scope
  out.newChildRecursive(undefined, rhs);

  // Optional comma
  while (pos.peek().text == ',') {
    pos.next();
  }

  pos.expect(']');
  return out;
}

/// e.g. "a.b.c.d.e" -> [a, b, c, d, e]
function parseIdentifier(pos: Pos): string[] {
  let out: string[] = [];

  // One name is required
  out.push(pos.next().text);

  // But any number more can follow that
  while (pos.peek().text == ".") {
    pos.next();
    out.push(pos.next().text);
  }

  return out;
}

function parseWeight(pos: Pos): number {
  let out = 0;
  if (pos.peek().text == '?') {
    while (pos.peek().text == '?') {
      --out;
      pos.next();
      if (pos.peek().text == '!') {
        throw new ScopeError('Cannot have both \'?\' and \'!\'',
                             pos.peek());
      }
    }
  } else if (pos.peek().text == '!') {
    while (pos.peek().text == '!') {
      ++out;
      pos.next();
      if (pos.peek().text == '?') {
        throw new ScopeError('Cannot have both \'?\' and \'!\'',
                             pos.peek());
      }
    }
  }
  return out;
}

/*
The RHS of a statement

Easy options:
- A scope / map
- A list

Easy + suffix options
- A variable name
- A lambda / arrow function
- A function call on some expressions

Hard:
- Math
*/
function parseExpression(pos: Pos): object {
  const tok = pos.peek();
  let obj: object = undefined;
  if (tok.text == '[') {
    // Array
    obj = parseArray(pos);
  } else if (tok.text == '{') {
    // Scope
    obj = parseScope(pos);
  }

  else if (tok.type == "ID") {
    // Identifier CHAIN
    obj = parseIdentifier(pos);
  } else if (tok.type == "LIT") {
    // Literal
    obj = tok;
  }

  // MATH GOES HERE!!!!!!!!!

  return obj;
}

////////////////////////////////////////////////////////////////

/// Parse from token stream
function parseJSONX(lexed: Token[]): Scope {
  return parseScope(new Pos(lexed));
}

export {Scope, parseJSONX};
