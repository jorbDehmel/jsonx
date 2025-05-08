'use strict';

/**
 * @file
 * @brief Lexing/tokenization for JSONX documents
 */

/// A single token in a token stream
class Token {
  /// The text of the token
  text: string;

  /// The type (e.g. ID, STRING, etc). Mostly unused
  type?: string;

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
    this.type = type;
    this.line = line;
    this.col = col;
  }
}

/// Tokenizer: converts source string into token list
function tokenize(src: string): Token[] {
  let tokens: Token[] = [];
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
  let match;
  while ((match = re.exec(src)) !== null) {
    if (match[1] || match[0].startsWith('//') ||
        match[0].startsWith('/*')) {
      continue; // skip whitespace/comments
    }
    const tk = match[0];
    tokens.push(new Token(tk));
  }
  tokens.push(new Token('EOF'));
  return tokens;
}

export {Token, tokenize};
