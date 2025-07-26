/**
 * @file
 * @brief Lexing/tokenization for JSONX documents
 */

import {JSONXVar} from "./parser";

/// LIT tokens will become blob literals (be they
/// strings, numbers, bools, etc) unless they are found to be
/// part of an identifier, OP tokens are syntax operators,
/// MATH tokens are mathematical operators, and ID tokens
/// refer to names. EOF tokens signify that we have passed the
/// end of the lex-ed token stream.
type TokenType = "LIT"|"OP"|"MATH"|"ID"|"EOF";

/// A single token in a token stream
class Token {
  /// The text of the token
  text: string;

  /// The type as determined at lex-time
  type: TokenType;

  /// The file of origin
  file?: string;

  /// The line in the file of origin
  line?: number;

  /// The column in the line of the file of origin
  col?: number;

  /// Construct with some optional junk, but at least text
  constructor(text: string, type?: TokenType, file?: string,
              line?: number, col?: number) {
    this.text = text;
    this.file = file;
    this.line = line;
    this.col = col;

    if (type !== undefined) {
      this.type = type;
    } else {
      // Attempt to categorize
      const operators = "{}[]():+-*/%<>!,?|&=.";

      // Null-like
      if (this.text == "" || this.text == "null") {
        this.type = "LIT";
      }

      // Booleans
      else if (this.text == "true" || this.text == "false") {
        this.type = "LIT";
      }

      // Strings
      else if (this.text.startsWith('"') ||
               this.text.startsWith("'") ||
               this.text.startsWith("`")) {
        this.type = "LIT";
      }

      // Operators
      else if (operators.includes(this.text[0])) {
        if (this.text == "+" || this.text == "-" ||
            this.text == "*" || this.text == "/" ||
            this.text == "%" || this.text == "&&" ||
            this.text == "||" || this.text == "==" ||
            this.text == "!=" || this.text == "<" ||
            this.text == "<=" || this.text == ">" ||
            this.text == ">=") {
          this.type = "MATH";
        } else {
          this.type = "OP";
        }
      }

      // EOF
      else if (this.text == "EOF") {
        this.type = "EOF";
      }

      // Else: Identifiers
      else {
        this.type = "ID";
      }
    }
  }
}

/// Tokenizer: converts source string into token list
function tokenize(src: string, filepath?: string): Token[] {
  const re = new RegExp(
      "([ \\t\\r\\n]+)|" +                       // Whitespace
          "(\\/\\/.*|\\/\\*[\\s\\S]*?\\*\\/)|" + // Comments
          "(" + // Non-junk stuff
          "[\\{\\}\\[\\]\\(\\):\\+\\-\\*\\/%<>!,;\\?]|" +
          "=>|\\+\\+|--|==|!=|===|!==|<=|>=|&&|\\|\\||" + // Ops
          "[A-Za-z_$][A-Za-z0-9_$]*|" + // Identifier
          "b16'([0-9A-Fa-f]+)'|b64'([A-Za-z0-9+/=]+)'|" + // Bin
          "`(?:\\.|\\$\\{|\\}|[^`])*`|" + // Format strings
          "'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\"|" + // Normal
                                                  // strings
          "\\.{3}|\\.|" + // Dots and ellipsis
          "[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?n?" + // Nums
          ")",
      "g");

  let col = 0;
  let line = 1;
  let tokens: Token[] = [];
  let match: RegExpExecArray|null;
  while ((match = re.exec(src)) !== null) {
    for (const c of match[0]) {
      if (c == '\n') {
        // Newlines count as commas

        ++line;
        col = 0;
      } else {
        ++col;
      }
    }

    if (match[1] || match[0].startsWith('//') ||
        match[0].startsWith('/*')) {
      continue; // skip whitespace/comments
    } else if (match[0] == ';') {
      // Semicolons count as commas
      tokens.push(
          new Token(',', undefined, filepath, line, col - 1));
    } else {
      const tk = match[0];
      tokens.push(new Token(tk, undefined, filepath, line,
                            col - tk.length));
    }
  }
  return tokens;
}

/// Represents a position in a token stream
class Pos {
  /// The list of tokens to iterate over
  tokens: Token[];

  /// The current index into `tokens`
  pos: number;

  ///
  replace(key: String, value: Pos): Pos {
    let out: Token[] = [];
    for (const tok of this.tokens) {
      if (tok.text == key) {
        for (const value_tok of value.tokens) {
          out.push(value_tok);
        }
      } else {
        out.push(tok);
      }
    }
    return new Pos(out);
  }

  /// Attach to some token stream
  constructor(tokens: Token[]) {
    this.tokens = tokens;
    this.pos = 0;
  }

  /// Get the current position in the array
  tell(): number {
    return this.pos;
  }

  /// Seek to some position
  seek(i: number) {
    this.pos = i;
  }

  /// Create a "child" position with a duplicate of some
  /// subsection of this object's array
  child(first: number, first_after: number): Pos {
    let l: Token[] = [];
    for (let i = first;
         0 <= i && i < this.tokens.length && i < first_after;
         ++i) {
      l.push(this.tokens[i]);
    }
    return new Pos(l);
  }

  /// Advance n tokens, default 1
  next(n: number = 1): void {
    this.pos += n;
  }

  /// Tell the current token
  cur(): Token {
    if (this.done()) {
      // Out of range!
      return new Token("EOF", "EOF");
    } else {
      // Not out of range.
      return this.tokens[this.pos];
    }
  }

  /// Returns whether we can call cur without EOF
  done(): boolean {
    return this.pos >= this.tokens.length;
  }

  /// Looks n tokens into the future. 0 would be cur()
  peek(n: number = 1): Token {
    if (this.pos + n >= this.tokens.length) {
      // Out of range!
      return new Token("EOF", "EOF");
    } else {
      // Not out of range.
      let tok = this.tokens[this.pos + n];
      return tok;
    }
  }
}

export {Token, tokenize, Pos};
