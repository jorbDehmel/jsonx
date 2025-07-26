/**
 * @brief Tests lambdas
 */

import {JSONX, JSONXBlob, JSONXLambdaBody} from '../src/parser';

function assertEq(exp: any, obs: any) {
  console.log(`Expected: ${exp}\nObserved: ${obs}\n`);
  if (exp != obs) {
    throw new Error('Failed equality assertion!');
  }
}

function main() {
  console.log('Running lambda tests...');

  let test1 = JSONX.loads(`
    {
      // Lambda that doesn't use its argument
      a: _ => y,
      a_call: a(foo),
      // Lambda that does use its argument
      b: x => x,
      b_call: b(z),
      // Currying
      c: x => y => x,
      c_call: c(alpha),
      c_call_call: c(alpha)(beta)
    }
  `);

  // Basic lambda returning a non-argument
  const a = test1.get('a') as JSONXLambdaBody;
  assertEq('_ => y', a.stringify());

  const a_call = test1.get('a_call');
  assertEq('y', a_call.stringify());
  assertEq('y', a.call(new JSONXBlob('foo')).stringify());

  const b_call = test1.get('b_call');
  assertEq('z', b_call.stringify());

  const c_call = test1.get('c_call');
  assertEq('y => alpha', c_call.stringify());

  const c_call_call = test1.get('c_call_call');
  assertEq('alpha', c_call_call.stringify());

  console.log('All lambda tests passed.');
}

main();
