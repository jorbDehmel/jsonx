"use strict";

/**
 * @file
 * @brief Forward-facing JSONX interface. This defines how to
 * resolve a JSONX object.
 */

import {tokenize} from "./lexer";
import {parseJSONX, Scope} from "./parser";

/**
 * @brief Load a JSONX-formatted string to a JS object
 * @param text The JSONX text to load
 * @param maxMs The max number of ms to give the process
 * @param maxBytesDA The max number of bytes dynamically
 *     allocatable
 * @returns The JS object represented by the JSONX text
 */
function loadsJSONX(text: string, maxMs?: number,
                    maxBytesDA?: number): object {
  // If a max time was given, start a timer
  let timeoutID: NodeJS.Timeout;
  if (maxMs != undefined) {
    timeoutID = setTimeout(
        () => {
          throw Error(
              `Exceeded loadsJSONX time limit of ${maxMs} ms`);
        },
    );
  }

  // Lex
  const tokens = tokenize(text);

  // Parse
  const parsed = parseJSONX(tokens);

  // Resolve
  const to_return = interpretJSONX(parsed, maxBytesDA);

  // If we have a timer running, cancel it
  if (maxMs != undefined) {
    clearTimeout(timeoutID);
  }
  return to_return;
}

export {loadsJSONX};
