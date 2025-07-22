/**
 * @brief A cli for testing
 */

import {JSONX} from "../src/parser";

function main() {
  // Read in a JSONX object from cin (until EOF)
  var jsonx = JSONX.loadf(0);

  // Print it out
  if (jsonx instanceof JSONX) {
    console.log(jsonx.stringify());
  } else {
    console.log(jsonx);
  }
}

main();
process.exit(0);
