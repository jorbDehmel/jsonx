"use strict";

/**
 * @file
 * @brief Forward-facing JSONX interface. This defines how to
 * resolve a JSONX object.
 */

import {tokenize} from "./lexer";
import {parseJSONX, Scope} from "./parser";

/**
 * @brief Resolve a parsed JSONX object.
 */
function interpretJSONX(what: Scope): object {
  return {};
}

////////////////////////////////////////////////////////////////

/// String to resolution
function loadsJSONX(text: string): object {
  const tokens = tokenize(text);
  const parsed = parseJSONX(tokens);
  return interpretJSONX(parsed);
}

export {loadsJSONX};
