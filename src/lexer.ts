'use strict';

/**
 * @file
 * @brief Lexing/tokenization for JSONX documents
 */

/// A single token in a token stream
class Token {
  /// The text of the token
  text: string;

  /// The type (LIT, NUM, OP, or ID)
  type: string;

  /// The file of origin
  file?: string;

  /// The line in the file of origin
  line?: number;

  /// The column in the line of the file of origin
  col?: number;

  /// Construct with some optional junk, but at least text
  constructor(text: string, type?: string, file?: string,
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

      // Numbers
      else if ('0' <= this.text[0] && this.text[0] <= '9') {
        this.type = "NUM";
      }

      // Operators
      else if (operators.includes(this.text[0])) {
        this.type = "OP";
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
          "[\\{\\}\\[\\]\\(\\):\\+\\-\\*\\/%<>!,\\?]|" + // Single
          "=>|\\+\\+|--|==|!=|===|!==|<=|>=|&&|\\|\\||" + // Ops
          "[A-Za-z_$][A-Za-z0-9_$]*|" + // Identifier
          "b16'([0-9A-Fa-f]+)'|b64'([A-Za-z0-9+/=]+)'|" + // Bin
          "`(?:\\.|\\$\\{|\\}|[^`])*`|" + // Format strings
          "'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\"|" + // Normal
                                                  // strings
          "\\.+|" + // Dots and elipsis
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
        ++line;
        col = 0;
      } else {
        ++col;
      }
    }

    if (match[1] || match[0].startsWith('//') ||
        match[0].startsWith('/*')) {
      continue; // skip whitespace/comments
    }
    const tk = match[0];
    tokens.push(new Token(tk, undefined, filepath, line,
                          col - tk.length));
  }
  tokens.push(new Token('EOF', 'EOF', filepath, line, col));
  return tokens;
}

export {Token, tokenize};
