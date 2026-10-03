import { describe, expect, it } from 'vitest';
import { capabilityTemplates, operationDeckDensity } from './capabilityModel';

describe('workflow capability model', () => {
  it('projects backend operation capabilities into node templates', () => {
    const templates = capabilityTemplates({
      operations: [
        {
          kind: 'operation',
          canonical_id: 'sf:operation',
          label: 'Operation',
          definition: 'Runs an operation.',
          project_entry: true,
          domain: 'core',
          module_id: 'core',
        },
      ],
    } as never);
    expect(templates[0]).toMatchObject({ id: 'sf:operation', capabilityId: 'sf:operation', projectEntry: true });
  });

  it('classifies operation deck density deterministically', () => {
    expect(operationDeckDensity('Short', 'short')).toBe('regular');
    expect(operationDeckDensity('A'.repeat(40), 'short')).toBe('dense');
  });
});
