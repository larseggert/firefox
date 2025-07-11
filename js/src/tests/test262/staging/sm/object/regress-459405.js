/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */

/*---
includes: [sm/non262.js, sm/non262-shell.js]
flags:
  - noStrict
description: |
  pending
esid: pending
---*/
//-----------------------------------------------------------------------------
var BUGNUMBER = 459405;
var summary = 'Math is not ReadOnly';
var actual = '';
var expect = '';


//-----------------------------------------------------------------------------
test();
//-----------------------------------------------------------------------------

function test()
{
  expect = 'foo';

  try
  {
    var Math = 'foo';
    actual = Math;
  }
  catch(ex)
  {
    actual = ex + '';
  }

  assert.sameValue(expect, actual, summary);
}

reportCompare(0, 0);
