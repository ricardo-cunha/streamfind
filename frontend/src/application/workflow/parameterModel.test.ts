import { describe, expect, it } from 'vitest';
import type { BackendCapability, CapabilityParameter } from '../../framework/backend/protocol';
import {
  defaultParameters,
  isFileListParameter,
  isJsonParameter,
  isPathListParameter,
  scalarInputValue,
  schemaTypeLabel,
  schemaPropertyOrder,
  uiParameters,
  wireTableToRows,
  wireParameters,
} from './parameterModel';

const numberParameter: CapabilityParameter = {
  name: 'threshold',
  label: 'Threshold',
  description: '',
  schema: { type: 'number' },
};

describe('workflow parameter model', () => {
  it('derives defaults and scalar input values from backend schemas', () => {
    const capability = { parameters: [numberParameter] } as BackendCapability;
    expect(defaultParameters(capability)).toEqual({ threshold: '' });
    expect(scalarInputValue(numberParameter, '2.5')).toBe(2.5);
  });

  it('formats nested schema labels and identifies path/json parameters', () => {
    expect(schemaTypeLabel({ type: 'array', items: { type: 'string' } })).toBe('array<string>');
    const pathParameter: CapabilityParameter = {
      ...numberParameter,
      schema: { type: 'array', items: { type: 'path', extensions: ['mzML'] } },
    };
    expect(isFileListParameter(pathParameter)).toBe(false);
    expect(isPathListParameter(pathParameter)).toBe(true);
    expect(isJsonParameter(pathParameter)).toBe(true);
  });

  it('normalizes persisted table values for the workflow editor', () => {
    expect(wireTableToRows({ columns: [{ name: 'mz', values: [100, 200] }] })).toEqual([{ mz: 100 }, { mz: 200 }]);
    expect(uiParameters(undefined, { value: 1 })).toEqual({ value: 1 });
    expect(
      wireParameters({ parameters: [{ name: 'known', schema: { type: 'string' } }] } as never, {
        known: 'value',
        stale: 'ignored',
      }),
    ).toEqual({ known: 'value' });
    expect(schemaPropertyOrder({ properties: { b: {}, a: {} }, 'x-streamfind-property-order': ['a'] })).toEqual([
      'a',
      'b',
    ]);
  });
});
