import { useEffect, useMemo, useRef } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import type { ArtifactDataResponse } from '../../../framework/backend/StreamFindApiClient';

export function VirtualArtifactTable({
  columns,
  pages,
  pageOffset,
  loading,
}: {
  columns: ArtifactDataResponse['columns'];
  pages: Record<number, ArtifactDataResponse>;
  pageOffset: number;
  loading: boolean;
}) {
  const page = pages[pageOffset];
  const rows = page?.rows ?? [];
  const scrollRef = useRef<HTMLDivElement | null>(null);
  const rowHeight = 34;
  const headerHeight = 48;
  const columnWidths = useMemo(
    () =>
      columns.map((column) => {
        const longestValue = (pages[pageOffset]?.rows ?? []).reduce((longest, row) => {
          const valueLength = String(row[column.name] ?? 'NULL').length;
          return Math.max(longest, valueLength);
        }, column.name.length);
        return Math.max(longestValue, column.type.length) * 8 + 32;
      }),
    [columns, pageOffset, pages],
  );
  // Only the active server-side page is kept in the vertical DOM window.
  // eslint-disable-next-line react-hooks/incompatible-library
  const rowVirtualizer = useVirtualizer({
    count: rows.length,
    getScrollElement: () => scrollRef.current,
    estimateSize: () => rowHeight,
    overscan: 8,
  });
  const columnVirtualizer = useVirtualizer({
    count: columns.length,
    getScrollElement: () => scrollRef.current,
    estimateSize: (index) => columnWidths[index] ?? 160,
    horizontal: true,
    overscan: 2,
  });
  useEffect(() => {
    columnWidths.forEach((width, index) => columnVirtualizer.resizeItem(index, width));
  }, [columnVirtualizer, columnWidths]);
  const virtualColumns = columnVirtualizer.getVirtualItems();
  const virtualRows = rowVirtualizer.getVirtualItems();
  const totalWidth = columnVirtualizer.getTotalSize();

  useEffect(() => {
    const scrollElement = scrollRef.current;
    if (!scrollElement || typeof scrollElement.scrollTo !== 'function') return;
    scrollElement.scrollTo({ top: 0, left: scrollElement.scrollLeft, behavior: 'auto' });
  }, [pageOffset]);

  return (
    <div
      ref={scrollRef}
      className="sf-artifact-virtual-scroll"
      role="table"
      aria-label="Artifact table"
      aria-rowcount={page?.total_rows ?? undefined}
      tabIndex={0}
    >
      {loading ? <div className="sf-artifact-viewer-loading">Loading rows…</div> : null}
      <div className="sf-artifact-virtual-header" style={{ width: totalWidth, height: headerHeight }}>
        {virtualColumns.map((virtualColumn) => {
          const column = columns[virtualColumn.index];
          return (
            <div
              className="sf-artifact-virtual-cell sf-artifact-virtual-header-cell"
              key={column.name}
              role="columnheader"
              style={{ left: virtualColumn.start, top: 0, width: virtualColumn.size, height: headerHeight }}
            >
              {column.name}
              <small>{column.type}</small>
            </div>
          );
        })}
      </div>
      <div className="sf-artifact-virtual-canvas" style={{ width: totalWidth, height: rowVirtualizer.getTotalSize() }}>
        {virtualRows.map((virtualRow) => {
          const row = rows[virtualRow.index];
          return virtualColumns.map((virtualColumn) => {
            const column = columns[virtualColumn.index];
            const cellValue = row ? (row[column.name] ?? 'NULL') : 'Loading…';
            return (
              <div
                className="sf-artifact-virtual-cell sf-artifact-virtual-body-cell"
                key={`${virtualRow.index}-${column.name}`}
                role="cell"
                style={{
                  left: virtualColumn.start,
                  top: virtualRow.start,
                  width: virtualColumn.size,
                  height: virtualRow.size,
                }}
              >
                <span title={cellValue}>{cellValue}</span>
              </div>
            );
          });
        })}
      </div>
      {!loading && rows.length === 0 ? <div className="sf-artifact-viewer-empty">No matching rows.</div> : null}
    </div>
  );
}
