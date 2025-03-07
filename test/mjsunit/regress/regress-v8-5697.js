// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --opt

function load(o) {
  return o.x;
};
%PrepareFunctionForOptimization(load);
for (var x = 0; x < 1000; ++x) {
  load({x});
  load({x});
  %OptimizeFunctionOnNextCall(load);
  try {
    load();
  } catch (e) {
  }
}

assertOptimized(load);

function store(o) {
  o.x = -1;
};
%PrepareFunctionForOptimization(store);
for (var x = 0; x < 1000; ++x) {
  store({x});
  store({x});
  %OptimizeFunctionOnNextCall(store);
  try {
    store();
  } catch (e) {
  }
}

assertOptimized(store);
