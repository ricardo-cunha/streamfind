import { useEffect, useRef, useState, type ReactNode } from 'react';

export type MassSpecFilterColumn = readonly [string, string];
export type MassSpecFilterValues = Record<string, string[]>;

type MassSpecFilterToolbarProps = {
  selectByColumns: readonly MassSpecFilterColumn[];
  filterColumns: readonly MassSpecFilterColumn[];
  showSelectByRow?: boolean;
  selectBy: string[];
  onSelectByChange: (columns: string[]) => void;
  optionValues: Record<string, string[]>;
  filters: MassSpecFilterValues;
  onFiltersChange: (filters: MassSpecFilterValues) => void;
  summary?: ReactNode;
};

function sortedOptions(values: string[]): string[] {
  return [...new Set(values)].sort((left, right) => left.localeCompare(right, undefined, { numeric: true }));
}

function updateSelection(values: string[], value: string, checked: boolean): string[] {
  if (checked) return values.includes(value) ? values : [...values, value];
  return values.filter((current) => current !== value);
}

function MultiValueFilter({
  column,
  label,
  options,
  selected,
  onChange,
}: {
  column: string;
  label: string;
  options: string[];
  selected: string[];
  onChange: (values: string[]) => void;
}) {
  const detailsRef = useRef<HTMLDetailsElement>(null);
  const [openDirection, setOpenDirection] = useState<'down' | 'up'>('down');
  const availableOptions = sortedOptions([...options, ...selected]);
  const summary = selected.length ? `${selected.length} selected` : 'All';

  useEffect(() => {
    const details = detailsRef.current;
    if (!details?.open) return undefined;

    const updateDirection = () => {
      const summaryElement = details.querySelector('summary');
      const menu = details.querySelector('.sf-mass-spec-filter-menu');
      if (!summaryElement || !menu) return;
      const summaryBounds = summaryElement.getBoundingClientRect();
      const menuHeight = menu.getBoundingClientRect().height;
      const spaceAbove = summaryBounds.top;
      const spaceBelow = window.innerHeight - summaryBounds.bottom;
      setOpenDirection(spaceBelow < menuHeight + 8 && spaceAbove > spaceBelow ? 'up' : 'down');
    };

    const frame = window.requestAnimationFrame(updateDirection);
    window.addEventListener('resize', updateDirection);
    window.addEventListener('scroll', updateDirection, true);
    return () => {
      window.cancelAnimationFrame(frame);
      window.removeEventListener('resize', updateDirection);
      window.removeEventListener('scroll', updateDirection, true);
    };
  });

  return (
    <details
      ref={detailsRef}
      className={`sf-mass-spec-filter sf-mass-spec-filter--${openDirection}`}
      data-filter-column={column}
    >
      <summary aria-label={`Filter by ${label}`}>
        <span>{label}</span>
        <span className="sf-mass-spec-filter-summary">{summary}</span>
      </summary>
      <div className="sf-mass-spec-filter-menu" role="group" aria-label={`Filter values for ${label}`}>
        <button
          type="button"
          className="sf-mass-spec-filter-clear"
          disabled={!selected.length}
          onClick={() => onChange([])}
        >
          All values
        </button>
        <div className="sf-mass-spec-filter-options">
          {availableOptions.length ? (
            availableOptions.map((option) => (
              <label key={option}>
                <input
                  type="checkbox"
                  checked={selected.includes(option)}
                  onChange={(event) => onChange(updateSelection(selected, option, event.target.checked))}
                />
                <span>{option}</span>
              </label>
            ))
          ) : (
            <span className="sf-mass-spec-filter-empty">No values available</span>
          )}
        </div>
      </div>
    </details>
  );
}

export function MassSpecFilterToolbar({
  selectByColumns,
  filterColumns,
  showSelectByRow = true,
  selectBy,
  onSelectByChange,
  optionValues,
  filters,
  onFiltersChange,
  summary,
}: MassSpecFilterToolbarProps) {
  return (
    <div className="sf-mass-spec-filter-toolbar">
      {showSelectByRow ? (
        <div className="sf-mass-spec-filter-row sf-mass-spec-select-by-row" aria-label="Select by">
          <span className="sf-mass-spec-filter-row-label">Select by</span>
          {selectByColumns.map(([column, label]) => (
            <label className="sf-mass-spec-checkbox" key={column}>
              <input
                type="checkbox"
                checked={selectBy.includes(column)}
                onChange={(event) => {
                  if (event.target.checked) {
                    onSelectByChange([...selectBy, column]);
                    return;
                  }
                  const next = selectBy.filter((value) => value !== column);
                  if (next.length) onSelectByChange(next);
                }}
              />
              {label}
            </label>
          ))}
        </div>
      ) : null}
      <div className="sf-mass-spec-filter-row sf-mass-spec-values-row" aria-label="Filters">
        <span className="sf-mass-spec-filter-row-label">Filter</span>
        {filterColumns.map(([column, label]) => (
          <MultiValueFilter
            key={column}
            column={column}
            label={label}
            options={optionValues[column] ?? []}
            selected={filters[column] ?? []}
            onChange={(values) => onFiltersChange({ ...filters, [column]: values })}
          />
        ))}
        {summary ? <span className="sf-mass-spec-filter-summary-text">{summary}</span> : null}
      </div>
    </div>
  );
}
