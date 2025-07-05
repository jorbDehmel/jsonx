
/**
 * @file
 * @brief Tests the JSONX parser
 */

import {JSONX} from "../src/parser_2";

/// Parses, then prints. Implicitly tests that the string
/// parses.
function parseAndPrint(text: string) {
  console.log(`Parsing raw text '${text}'`);
  let s = JSONX.loads(text);
  console.log('Parsed:');
  console.log(s);
  console.log();
}

/// Runs test cases
function main() {
  // Null case
  parseAndPrint('');

  // Unit object
  parseAndPrint('{}');

  // Single value
  parseAndPrint('true');

  // Simple scopes
  parseAndPrint('{a: 12}');
  parseAndPrint('{a: 12, b: 13}');

  // Remember, this should be a superset of JSON
  parseAndPrint('{"a": 12, "b": 13}');

  // Simple array
  parseAndPrint('[1, true, a, 100]');

  // Lambdas
  parseAndPrint(
      '{api: endpoint => `https://api.com/\${endpoint}`, ' +
      'x: y, x?: 33, x!!!!: 7}');

  // Weights, math, and compound structures
  parseAndPrint('{a!!!: {b?: 123}, c: a.0}');
  parseAndPrint('[a, b, c, {d: 12, e: f}, g]');
  parseAndPrint('{a: 123 + 5 * 16, b: a == 100 + 2}');

  // Complex names and nestings
  parseAndPrint(
      '{api : {}, false: true, "false": this.false, ":": ' +
      '"false"."false", defaults: {x: 3, y: 4, z: parent.z}, ' +
      'y: defaults.y + 1, y!!!!: 77 "z": 44} == 77');
}

main();
