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
  console.log(`Parsing '${text}'`);

  console.log('Tokenized:\n')
  tokenize(text).forEach((what) => {
    console.log(what);
  });

  let s: Scope = parseJSONX(tokenize(text));
  console.log(s);
}

/// Runs test cases
function main() {
  parseAndPrint('{a: 12}');
}

main();
