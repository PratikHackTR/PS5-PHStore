'use strict';
const assert = require('node:assert/strict');
const { windowFor, columnsFromGridTemplate, adjacentIndex } = require('../frontend/src/virtual-grid.js');

assert.equal(columnsFromGridTemplate('240px 240px 240px 240px'), 4);
assert.equal(columnsFromGridTemplate('none'), 1);

const initial = windowFor(865, 5, 500, 0, 900, 2);
assert.equal(initial.firstIndex, 0);
assert.ok(initial.endIndex < 865);
assert.ok(initial.endIndex / 5 <= 7, 'visible rows include only viewport plus 2 row overscan');

const middle = windowFor(865, 5, 500, 40500, 900, 2);
assert.equal(middle.firstIndex, middle.firstRow * 5);
assert.ok(middle.firstIndex > 300 && middle.firstIndex < 500);
assert.ok(middle.top > 0 && middle.bottom > 0);
assert.equal(initial.top, 0);
assert.equal(windowFor(7, 5, 500, 0, 900, 2).endIndex, 7);

assert.equal(adjacentIndex(6, 'ArrowRight', 5, 20), 7);
assert.equal(adjacentIndex(4, 'ArrowRight', 5, 20), -1, 'right arrow must not wrap rows');
assert.equal(adjacentIndex(6, 'ArrowUp', 5, 20), 1);
assert.equal(adjacentIndex(16, 'ArrowDown', 5, 20), -1);
assert.equal(adjacentIndex(0, 'ArrowLeft', 5, 20), -1);
console.log('virtual_grid=PASS (window bounds, overscan, resize columns, edge navigation)');
