"use strict";

import {tokenize, Token} from "../src/lexer";
import {parseJSONX, Scope} from "../src/parser";

/**
 * @file
 * @brief Tests the JSONX parser
 */

/// Runs test cases
function main() {
  let s: Scope = parseJSONX(tokenize(''));
  console.log(s);
}

main();
