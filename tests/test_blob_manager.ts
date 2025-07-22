/**
 * @file
 * @brief Tests the BlobManager and BlobInstance classes
 */

import {JSONXBlob} from "../src/blob";

function assert(condition: boolean, msg?: string): void {
  if (!condition) {
    throw new Error(msg == undefined ? 'Assertion failed'
                                     : msg);
  }
}

/// Run test cases
function main() {
  console.log('Running test_blob_manager tests...');

  JSONXBlob.maxBytes = 8;
  assert(JSONXBlob.bytesUsed == 0);

  let a = new JSONXBlob();
  assert(JSONXBlob.bytesUsed == 0);

  // Allocate all the allowed memory
  a.set(JSONXBlob.encode("Hi there"));
  assert(JSONXBlob.bytesUsed == 8);

  // Should still be at 8 bytes usage after this
  a.free();
  assert(JSONXBlob.bytesUsed == 0);

  // Should throw an error
  let didFail = false;
  try {
    a.set(JSONXBlob.encode("Alabama banana charlie doughnut"));
  } catch {
    didFail = true;
  }
  assert(didFail, 'Expected out-of-mem, but saw none!');

  // This will have deallocated `a` upon error

  console.log("All BlobManager unit tests passed.");
}

main();
