"use strict";

import {tokenize} from "../src/lexer";
import {parseJSONX, Scope} from "../src/parser";

/**
 * @file
 * @brief Tests the JSONX parser
 */

/// Parses, then prints. Implicitly tests that the string
/// parses.
function parseAndPrint(text: string) {
  console.log(`Parsing raw text '${text}'`);

  console.log('Tokenized:')
  tokenize(text).forEach((what) => {
    console.log(what);
  });

  let s = parseJSONX(tokenize(text));
  console.log('Parsed:');
  console.log(s);
  console.log();
}

/// Runs test cases
function main() {
  parseAndPrint('true');
  parseAndPrint('{a: 12}');
  parseAndPrint('[a, b, c, {d: 12, e: f}]');
}

main();
