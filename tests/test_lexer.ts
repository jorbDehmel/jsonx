"use strict";

/**
 * @file
 * @brief Tests the JSONX lexer/tokenizer
 */

import {Token, tokenize} from "../src/lexer";

/// Assert that the lexed version of toLex matches expected
function testCase(toLex: string, expected: string[]) {
  let observedToks: Token[] = tokenize(toLex);
  let observed: string[] = [];
  observedToks.forEach((tok) => {
    observed.push(tok.text);
  });

  if (observed.length != expected.length) {
    console.log("Expected:");
    console.log(expected);

    console.log("Observed:");
    console.log(observed);
    throw new Error('Failed match length!');
  }
  for (let i = 0; i < expected.length; ++i) {
    if (expected[i] !== observed[i]) {
      console.log("Expected:");
      console.log(expected);

      console.log("Observed:");
      console.log(observed);
      throw new Error('Failed match value!');
    }
  }

  console.log(`Test case '${toLex}' passed`);
}

/// Run several test cases
function main() {
  // Empty document
  testCase('', [ 'EOF' ]);

  // Document with empty object
  testCase('{}', [ '{', '}', 'EOF' ]);

  // Simple mapping w/ spacing
  testCase('{ false : "true" }',
           [ '{', 'false', ':', '"true"', '}', 'EOF' ]);

  // Simple mapping w/o spacing
  testCase('{false: "true"}',
           [ '{', 'false', ':', '"true"', '}', 'EOF' ]);

  // Complex mapping w/ array, names, math
  testCase('alabama: [1, 2, {banana: clown + 1, clown: 12}]', [
    'alabama', ':',      '[',  '1',     ',', '2',  ',',
    '{',       'banana', ':',  'clown', '+', '1',  ',',
    'clown',   ':',      '12', '}',     ']', 'EOF'
  ]);

  // Lambda object w/ default args, free variables and complex
  // math
  testCase('lambda_haver: {data: null, ' +
               'op: {a: 2, b: 3} => data || b * c == d}, ' +
               'foo: lambda_haver.op({5, c * 2})',
           [
             'lambda_haver',
             ':',
             '{',
             'data',
             ':',
             'null',
             ',',
             'op',
             ':',
             '{',
             'a',
             ':',
             '2',
             ',',
             'b',
             ':',
             '3',
             '}',
             '=>',
             'data',
             '||',
             'b',
             '*',
             'c',
             '==',
             'd',
             '}',
             ',',
             'foo',
             ':',
             'lambda_haver',
             '.',
             'op',
             '(',
             '{',
             '5',
             ',',
             'c',
             '*',
             '2',
             '}',
             ')',
             'EOF'
           ]);

  // Weighted object + hierarchical names
  testCase('a!!!: {b?: 123}, c: a.0', [
    'a', '!', '!', '!', ':', '{', 'b', '?', ':', '123', '}',
    ',', 'c', ':', 'a', '.', '0', 'EOF'
  ]);
}

main();
