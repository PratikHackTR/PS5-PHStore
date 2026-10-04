(function (root) {
  'use strict';

  function windowFor(totalItems, columns, rowHeight, scrollTop, viewportHeight, overscanRows) {
    const count = Math.max(0, Math.floor(Number(totalItems) || 0));
    const cols = Math.max(1, Math.floor(Number(columns) || 1));
    const height = Math.max(1, Number(rowHeight) || 1);
    const totalRows = Math.ceil(count / cols);
    const firstVisibleRow = Math.max(0, Math.floor(Math.max(0, Number(scrollTop) || 0) / height));
    const visibleRows = Math.max(1, Math.ceil(Math.max(1, Number(viewportHeight) || 1) / height));
    const overscan = Math.max(0, Math.floor(Number(overscanRows) || 0));
    const firstRow = Math.max(0, firstVisibleRow - overscan);
    const endRow = Math.min(totalRows, firstVisibleRow + visibleRows + overscan + 1);
    return {
      firstRow,
      endRow,
      firstIndex: Math.min(count, firstRow * cols),
      endIndex: Math.min(count, endRow * cols),
      top: firstRow * height,
      bottom: Math.max(0, (totalRows - endRow) * height),
      totalRows,
      columns: cols
    };
  }

  function columnsFromGridTemplate(template) {
    const value = String(template || '').trim();
    if (!value || value === 'none') return 1;
    return value.split(/\s+/).length;
  }

  function adjacentIndex(index, direction, columns, totalItems) {
    const at = Math.floor(Number(index));
    const cols = Math.max(1, Math.floor(Number(columns) || 1));
    const total = Math.max(0, Math.floor(Number(totalItems) || 0));
    let target = at;
    if (direction === 'ArrowLeft' && at % cols > 0) target = at - 1;
    else if (direction === 'ArrowRight' && at % cols < cols - 1) target = at + 1;
    else if (direction === 'ArrowUp') target = at - cols;
    else if (direction === 'ArrowDown') target = at + cols;
    return target >= 0 && target < total && target !== at ? target : -1;
  }

  const api = { windowFor, columnsFromGridTemplate, adjacentIndex };
  root.PHStoreVirtualGrid = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
