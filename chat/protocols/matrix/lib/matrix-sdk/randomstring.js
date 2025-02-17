"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.randomLowercaseString = randomLowercaseString;
exports.randomString = randomString;
exports.randomUppercaseString = randomUppercaseString;
/*
Copyright 2018 New Vector Ltd
Copyright 2019 The Matrix.org Foundation C.I.C.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

const LOWERCASE = "abcdefghijklmnopqrstuvwxyz";
const UPPERCASE = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const DIGITS = "0123456789";
function randomString(len) {
  return randomStringFrom(len, UPPERCASE + LOWERCASE + DIGITS);
}
function randomLowercaseString(len) {
  return randomStringFrom(len, LOWERCASE);
}
function randomUppercaseString(len) {
  return randomStringFrom(len, UPPERCASE);
}
function randomStringFrom(len, chars) {
  let ret = "";
  for (let i = 0; i < len; ++i) {
    ret += chars.charAt(Math.floor(Math.random() * chars.length));
  }
  return ret;
}