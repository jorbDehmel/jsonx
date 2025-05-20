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
             '=>',  '...',     '}',    'EOF'
           ]);

  testCase('true', [ 'true', 'EOF' ]);

  testCase('.......', [ '...', '...', '.', 'EOF' ]);

  testCase('2*acos(0) // pi',
           [ '2', '*', 'acos', '(', '0', ')', 'EOF' ]);

  testCase('read(\'aes.key\') // data as binary blob',
           [ 'read', '(', '\'aes.key\'', ')', 'EOF' ]);

  testCase(
      'encrypt(), decrypt()',
      [ 'encrypt', '(', ')', ',', 'decrypt', '(', ')', 'EOF' ]);

  testCase('use(std)', [ 'use', '(', 'std', ')', 'EOF' ]);

  testCase('b16(\'dead.beef\')',
           [ 'b16', '(', '\'dead.beef\'', ')', 'EOF' ]);

  testCase(
      'int(b10(\'32\'))',
      [ 'int', '(', 'b10', '(', '\'32\'', ')', ')', 'EOF' ]);

  testCase('b16.decode(\'dead.beef\')', [
    'b16', '.', 'decode', '(', '\'dead.beef\'', ')', 'EOF'
  ]);

  testCase('b64.decode(\'peas.and.carrots\')', [
    'b64', '.', 'decode', '(', '\'peas.and.carrots\'', ')',
    'EOF'
  ]);

  testCase('int.decode(\'dead.beef\')', [
    'int', '.', 'decode', '(', '\'dead.beef\'', ')', 'EOF'
  ]);

  testCase('uint()', [ 'uint', '(', ')', 'EOF' ]);

  testCase('int(\'32\')', [ 'int', '(', '\'32\'', ')', 'EOF' ]);

  testCase('float(\'32\')',
           [ 'float', '(', '\'32\'', ')', 'EOF' ]);

  testCase('byte_size(\'apple\')',
           [ 'byte_size', '(', '\'apple\'', ')', 'EOF' ]);

  testCase('char_size(\'apple\')',
           [ 'char_size', '(', '\'apple\'', ')', 'EOF' ]);

  testCase('{ x : 3 , y : 7}.x', [
    '{', 'x', ':', '3', ',', 'y', ':', '7', '}', '.', 'x', 'EOF'
  ]);

  testCase('number.mantessa',
           [ 'number', '.', 'mantessa', 'EOF' ]);

  testCase('number.base', [ 'number', '.', 'base', 'EOF' ]);

  testCase('number.exponent',
           [ 'number', '.', 'exponent', 'EOF' ]);

  testCase('integer : number { number.exponent => 0 }', [
    'integer', ':', 'number', '{', 'number', '.', 'exponent',
    '=>', '0', '}', 'EOF'
  ]);

  testCase('x.foo()', [ 'x', '.', 'foo', '(', ')', 'EOF' ]);

  testCase('x : me.x', [ 'x', ':', 'me', '.', 'x', 'EOF' ]);

  testCase('y : me.y', [ 'y', ':', 'me', '.', 'y', 'EOF' ]);

  testCase('.1', [ '.', '1', 'EOF' ]);

  // Tricky one!
  testCase('.1 .2 .3 .5',
           [ '.', '1', '.', '2', '.', '3', '.', '5', 'EOF' ]);

  testCase('0.1', [ '0.1', 'EOF' ]);

  testCase('[[1,3,]].0 .1', [
    '[', '[', '1', ',', '3', ',', ']', ']', '.', '0', '.', '1',
    'EOF'
  ]);

  testCase('[x,y,z]',
           [ '[', 'x', ',', 'y', ',', 'z', ']', 'EOF' ]);

  testCase('44.5', [ '44.5', 'EOF' ]);

  testCase('json!?', [ 'json', '!', '?', 'EOF' ]);

  testCase('{use(std), x: 1, pi: 2 * atan(1),}', [
    '{',    'use', '(', 'std', ')', ',', 'x',
    ':',    '1',   ',', 'pi',  ':', '2', '*',
    'atan', '(',   '1', ')',   ',', '}', 'EOF'
  ]);

  testCase('{x : "y" + .y, y:.z + \'1\', z: 3, ' +
               'a.b.c: 3, a: {b: {c: 3}}}',
           [
             '{', 'x', ':', '"y"', '+', '.',     'y', ',',
             'y', ':', '.', 'z',   '+', '\'1\'', ',', 'z',
             ':', '3', ',', 'a',   '.', 'b',     '.', 'c',
             ':', '3', ',', 'a',   ':', '{',     'b', ':',
             '{', 'c', ':', '3',   '}', '}',     '}', 'EOF'
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
        '}', '==',      '77',    'EOF'
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
        '==',       '7',
        'EOF'
      ]);

  console.log('All lexer unit tests passed.');
}

main();
