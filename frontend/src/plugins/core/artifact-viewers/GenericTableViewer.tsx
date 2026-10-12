import { useCallback, useEffect, useRef, useState } from 'react';
import type { ViewerComponentProps } from '../../../framework/viewers/viewerTypes';
import type { ArtifactDataResponse } from '../../../framework/backend/StreamFindApiClient';
import { VirtualArtifactTable } from './VirtualArtifactTable';

const PAGE_SIZE_OPTIONS = [10, 25, 50, 100, 250, 500, 1000];

export function GenericTableViewer({ context }: ViewerComponentProps) {
  const artifact = context.artifact;
  const client = context.pluginApi?.client;
  const [data, setData] = useState<ArtifactDataResponse | null>(null);
  const [pages, setPages] = useState<Record<number, ArtifactDataResponse>>({});
  const [search, setSearch] = useState('');
  const [pageSize, setPageSize] = useState(250);
  const [sort, setSort] = useState('');
  const [descending, setDescending] = useState(false);
  const [loading, setLoading] = useState(false);
  const requests = useRef(new Set<number>());
  const queryRef = useRef('');

  const requestPage = useCallback(
    (offset: number, activate: boolean) => {
      if (!artifact || artifact.representation !== 'table' || !client) return;
      const query = `${artifact.artifact_id}|${pageSize}|${search}|${sort}|${descending}`;
      if (offset !== 0 && queryRef.current !== query) return;
      if (offset === 0 && queryRef.current !== query) {
        queryRef.current = query;
        requests.current.clear();
        setData(null);
        setPages({});
      }
      if (requests.current.has(offset)) return;
      requests.current.add(offset);
      setLoading(true);
      void client
        .artifactData(context.sessionId, {
          artifact_id: artifact.artifact_id,
          limit: pageSize,
          offset,
          search,
          sort_column: sort,
          descending,
        })
        .then((result) => {
          if (queryRef.current !== query) return;
          if (activate) setData(result);
          setPages((current) => ({ ...current, [offset]: result }));
        })
        .catch(() => undefined)
        .finally(() => {
          requests.current.delete(offset);
          if (queryRef.current === query && requests.current.size === 0) setLoading(false);
        });
    },
    [artifact, client, context.sessionId, descending, pageSize, search, sort],
  );

  useEffect(() => {
    queryRef.current = '';
    requestPage(0, true);
  }, [requestPage]);

  if (!artifact || !client) return <div role="alert">Table artifact data is unavailable.</div>;
  const columns = data?.columns ?? artifact.columns ?? [];
  const offset = data?.offset ?? 0;
  const totalRows = data?.total_rows ?? 0;

  return (
    <>
      <div className="sf-artifact-viewer-toolbar">
        <label>
          Rows per page
          <select
            value={pageSize}
            onChange={(event) => {
              setPageSize(Number(event.target.value));
              setData(null);
            }}
            aria-label="Rows per page"
          >
            {PAGE_SIZE_OPTIONS.map((size) => (
              <option key={size} value={size}>
                {size}
              </option>
            ))}
          </select>
        </label>
        <input
          value={search}
          onChange={(event) => {
            setSearch(event.target.value);
            setData(null);
          }}
          placeholder="Search all columns"
          aria-label="Search all columns"
        />
        <select
          value={sort}
          onChange={(event) => {
            setSort(event.target.value);
            setData(null);
          }}
          aria-label="Sort by column"
        >
          <option value="">Natural order</option>
          {columns.map((column) => (
            <option key={column.name} value={column.name}>
              {column.name}
            </option>
          ))}
        </select>
        <button type="button" onClick={() => setDescending((value) => !value)} disabled={!sort}>
          {descending ? 'Descending' : 'Ascending'}
        </button>
      </div>
      <VirtualArtifactTable columns={columns} pages={pages} pageOffset={offset} loading={loading} />
      <footer>
        <span>
          {data
            ? `Showing rows ${offset + 1}-${Math.min(offset + (data.rows?.length ?? 0), totalRows)} of ${totalRows}`
            : 'No rows loaded'}
        </span>
        <button
          type="button"
          onClick={() => requestPage(Math.max(0, offset - pageSize), true)}
          disabled={!data || offset === 0 || loading}
        >
          Previous page
        </button>
        <button
          type="button"
          onClick={() => requestPage(offset + pageSize, true)}
          disabled={!data || offset + (data.rows?.length ?? 0) >= totalRows || loading}
        >
          Next page
        </button>
      </footer>
    </>
  );
}
