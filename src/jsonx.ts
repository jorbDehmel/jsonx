/**
 * @file
 * @brief Forward-facing JSONX interface. This defines how to
 * resolve a JSONX object.
 */

import {BlobManager} from "./blob_manager";
import {Token, tokenize} from "./lexer";
import {JSONX, JSONXVarType, parseJSONX} from "./parser";

/**
 * @brief Load a JSONX-formatted string to a JS object
 * @param text The JSONX text to load
 * @param maxMs The max number of ms to give the process
 * @param maxBytesDA The max number of bytes dynamically
 *     allocatable
 * @returns The JS object represented by the JSONX text
 */
function loadsJSONX(text: string, maxMs?: number,
                    maxBytesDA?: number): JSONXVarType {
  // Set max bytes
  BlobManager.maxBytes = maxBytesDA;

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

  // If we have a timer running, cancel it
  if (maxMs != undefined) {
    clearTimeout(timeoutID);
  }
  return parsed;
}

export {loadsJSONX, JSONX};
