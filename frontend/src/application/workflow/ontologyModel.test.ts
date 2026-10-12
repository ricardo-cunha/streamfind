import { describe, expect, it } from 'vitest';
import { parameterOntologyDetails, portOntologyDetails, schemaOntologyDetails } from './ontologyModel';

describe('workflow ontology model', () => {
  it('extracts schema metadata', () => {
    expect(schemaOntologyDetails({ type: 'string', path_kind: 'file', extensions: ['mzML'] })).toEqual([
      'path kind: file',
      'extensions: mzML',
    ]);
  });

  it('deduplicates parameter metadata', () => {
    expect(
      parameterOntologyDetails({
        name: 'input',
        label: 'Input',
        schema: { type: 'array', items: { type: 'string', path_kind: 'file' } },
        path_kind: 'file',
        extensions: ['csv'],
      } as never),
    ).toEqual(['path kind: file', 'allowed extensions: csv']);
  });

  it('describes a port using its semantic table identity', () => {
    expect(portOntologyDetails({ schema: { type: 'string' }, dataKind: 'value' } as never)).toEqual(['type: string']);
  });
});
