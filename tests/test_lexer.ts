/**
 * @file
 * @brief Tests the JSONX lexer/tokenizer
 */

import {tokenize} from "../src/lexer";

/// Assert that the lexed version of toLex matches expected
function testCase(toLex: string, expected: string[]) {
  let observedToks = tokenize(toLex);
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
  console.log('Running test_lexer test cases...');

  // Empty document
  testCase('', []);

  // Document with empty object
  testCase('{}', [ '{', '}' ]);

  // Simple mapping w/ spacing
  testCase('{ false : "true" }',
           [ '{', 'false', ':', '"true"', '}' ]);

  // Simple mapping w/o spacing
  testCase('{false: "true"}',
           [ '{', 'false', ':', '"true"', '}' ]);

  // Complex mapping w/ array, names, math
  testCase('alabama: [1, 2, {banana: clown + 1, clown: 12}]', [
    'alabama', ':', '[', '1', ',', '2', ',', '{', 'banana', ':',
    'clown', '+', '1', ',', 'clown', ':', '12', '}', ']'
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
             ')'
           ]);

  // Weighted object + hierarchical names
  testCase('a!!!: {b?: 123}, c: a.0', [
    'a', '!', '!', '!', ':', '{', 'b', '?', ':', '123', '}',
    ',', 'c', ':', 'a', '.', '0'
  ]);

  //////////////////////////////////////////////////////////////
  // Test cases from ../examples.jsonx

  testCase(`
      std : {
        acos : x => std.acos(x),
        read: file => std.read(file),
        encrypt : { message : ? , key : ? } => ...
      }`,
           [
             'std', ':',       '{',    'acos', ':',       'x',
             '=>',  'std',     '.',    'acos', '(',       'x',
             ')',   ',',       'read', ':',    'file',    '=>',
             'std', '.',       'read', '(',    'file',    ')',
             ',',   'encrypt', ':',    '{',    'message', ':',
             '?',   ',',       'key',  ':',    '?',       '}',
             '=>',  '...',     '}'
           ]);

  testCase('true', [ 'true' ]);

  testCase('.......', [ '...', '...', '.' ]);

  testCase('2*acos(0) // pi',
           [ '2', '*', 'acos', '(', '0', ')' ]);

  testCase('read(\'aes.key\') // data as binary blob',
           [ 'read', '(', '\'aes.key\'', ')' ]);

  testCase('encrypt(), decrypt()',
           [ 'encrypt', '(', ')', ',', 'decrypt', '(', ')' ]);

  testCase('use(std)', [ 'use', '(', 'std', ')' ]);

  testCase('b16(\'dead.beef\')',
           [ 'b16', '(', '\'dead.beef\'', ')' ]);

  testCase('int(b10(\'32\'))',
           [ 'int', '(', 'b10', '(', '\'32\'', ')', ')' ]);

  testCase('b16.decode(\'dead.beef\')',
           [ 'b16', '.', 'decode', '(', '\'dead.beef\'', ')' ]);

  testCase('b64.decode(\'peas.and.carrots\')', [
    'b64', '.', 'decode', '(', '\'peas.and.carrots\'', ')'
  ]);

  testCase('int.decode(\'dead.beef\')',
           [ 'int', '.', 'decode', '(', '\'dead.beef\'', ')' ]);

  testCase('uint()', [ 'uint', '(', ')' ]);

  testCase('int(\'32\')', [ 'int', '(', '\'32\'', ')' ]);

  testCase('float(\'32\')', [ 'float', '(', '\'32\'', ')' ]);

  testCase('byte_size(\'apple\')',
           [ 'byte_size', '(', '\'apple\'', ')' ]);

  testCase('char_size(\'apple\')',
           [ 'char_size', '(', '\'apple\'', ')' ]);

  testCase('{ x : 3 , y : 7}.x', [
    '{', 'x', ':', '3', ',', 'y', ':', '7', '}', '.', 'x'
  ]);

  testCase('number.mantessa', [ 'number', '.', 'mantessa' ]);

  testCase('number.base', [ 'number', '.', 'base' ]);

  testCase('number.exponent', [ 'number', '.', 'exponent' ]);

  testCase('integer : number { number.exponent => 0 }', [
    'integer', ':', 'number', '{', 'number', '.', 'exponent',
    '=>', '0', '}'
  ]);

  testCase('x.foo()', [ 'x', '.', 'foo', '(', ')' ]);

  testCase('x : me.x', [ 'x', ':', 'me', '.', 'x' ]);

  testCase('y : me.y', [ 'y', ':', 'me', '.', 'y' ]);

  testCase('.1', [ '.', '1' ]);

  // Tricky one!
  testCase('.1 .2 .3 .5',
           [ '.', '1', '.', '2', '.', '3', '.', '5' ]);

  testCase('0.1', [ '0.1' ]);

  testCase('[[1,3,]].0 .1', [
    '[', '[', '1', ',', '3', ',', ']', ']', '.', '0', '.', '1'
  ]);

  testCase('[x,y,z]', [ '[', 'x', ',', 'y', ',', 'z', ']' ]);

  testCase('44.5', [ '44.5' ]);

  testCase('json!?', [ 'json', '!', '?' ]);

  testCase('{use(std), x: 1, pi: 2 * atan(1),}', [
    '{',  'use', '(', 'std', ')',    ',', 'x', ':', '1', ',',
    'pi', ':',   '2', '*',   'atan', '(', '1', ')', ',', '}'
  ]);

  testCase('{x : "y" + .y, y:.z + \'1\', z: 3, ' +
               'a.b.c: 3, a: {b: {c: 3}}}',
           [
             '{', 'x', ':', '"y"', '+', '.',     'y', ',',
             'y', ':', '.', 'z',   '+', '\'1\'', ',', 'z',
             ':', '3', ',', 'a',   '.', 'b',     '.', 'c',
             ':', '3', ',', 'a',   ':', '{',     'b', ':',
             '{', 'c', ':', '3',   '}', '}',     '}'
           ]);

  testCase(
      '{ api :{}, false : true "false":.false, ":": ' +
          '"false"."false" defaults: {x: 3, y: 4, z:super.z}, y: ' +
          'defaults.y + 1, y!!!!: 77 "z": 44} == 77',
      [
        '{', 'api',     ':',     '{',       '}',
        ',', 'false',   ':',     'true',    '"false"',
        ':', '.',       'false', ',',       '":"',
        ':', '"false"', '.',     '"false"', 'defaults',
        ':', '{',       'x',     ':',       '3',
        ',', 'y',       ':',     '4',       ',',
        'z', ':',       'super', '.',       'z',
        '}', ',',       'y',     ':',       'defaults',
        '.', 'y',       '+',     '1',       ',',
        'y', '!',       '!',     '!',       '!',
        ':', '77',      '"z"',   ':',       '44',
        '}', '==',      '77'
      ]);

  testCase(
      '{ api : (endpoint) => ' +
          '\`https://api.com/\${endpoint}\`, x: y, x?: 33, ' +
          'x ?? ? !!!! : 7, }.x == 7 ',
      [
        '{',        'api',
        ':',        '(',
        'endpoint', ')',
        '=>',       '\`https://api.com/\${endpoint}\`',
        ',',        'x',
        ':',        'y',
        ',',        'x',
        '?',        ':',
        '33',       ',',
        'x',        '?',
        '?',        '?',
        '!',        '!',
        '!',        '!',
        ':',        '7',
        ',',        '}',
        '.',        'x',
        '==',       '7'
      ]);

  console.log('All lexer unit tests passed.');
}

main();
