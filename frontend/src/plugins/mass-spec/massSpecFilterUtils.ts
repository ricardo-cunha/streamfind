import type { ResultQueryFilter } from '../../framework/backend/StreamFindApiClient';

const MZ_FILTER_COLUMNS = new Set(['precursor_mz', 'product_mz']);
const NUMERIC_FILTER_COLUMNS = new Set(['precursor_mz', 'product_mz', 'wavelength_nm', 'polarity']);
const MZ_FILTER_TOLERANCE_PPM = 2;
const MIN_MZ_FILTER_TOLERANCE = 0.0001;

export function isNumericFilterColumn(column: string): boolean {
  return NUMERIC_FILTER_COLUMNS.has(column);
}

function mzTolerance(value: number): number {
  return Math.max(MIN_MZ_FILTER_TOLERANCE, Math.abs(value) * MZ_FILTER_TOLERANCE_PPM * 1e-6);
}

export function formatFilterValue(column: string, value: unknown): string {
  if (value === null || value === undefined) return '(empty)';
  if (MZ_FILTER_COLUMNS.has(column)) {
    const numeric = Number(value);
    if (Number.isFinite(numeric)) return Number(numeric.toFixed(2)).toString();
  }
  return String(value);
}

export function filterValueMatches(column: string, rowValue: string, selectedValues: string[]): boolean {
  if (!selectedValues.length) return true;
  if (!MZ_FILTER_COLUMNS.has(column)) return selectedValues.includes(rowValue || '(empty)');
  const numericRowValue = Number(rowValue);
  if (!Number.isFinite(numericRowValue)) return selectedValues.includes('(empty)');
  return selectedValues.some((selectedValue) => {
    const numericSelectedValue = Number(selectedValue);
    return Number.isFinite(numericSelectedValue) &&
      Math.abs(numericRowValue - numericSelectedValue) <= mzTolerance(numericSelectedValue);
  });
}

export function buildResultQueryFilter(column: string, values: string[]): ResultQueryFilter {
  if (!isNumericFilterColumn(column)) {
    return values.length === 1
      ? { column, operator: 'eq', value: values[0] }
      : { column, operator: 'in', values };
  }

  const numericValues = values.map(Number).filter(Number.isFinite);
  if (MZ_FILTER_COLUMNS.has(column) && numericValues.length) {
    const tolerance = Math.max(...numericValues.map(mzTolerance));
    return {
      column,
      operator: 'between',
      min: Math.min(...numericValues) - tolerance,
      max: Math.max(...numericValues) + tolerance,
    };
  }

  return numericValues.length === 1
    ? { column, operator: 'eq', value: numericValues[0] }
    : { column, operator: 'in', values: numericValues };
}
