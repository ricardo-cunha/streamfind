import { describe, expect, it } from 'vitest';
import { nodePorts, parameterTypeKey, portTypeKey, schemaShape, typeClass, typeIcon } from './portModel';

describe('workflow port model', () => {
  it('normalizes backend ports and derives stable type metadata', () => {
    const capability = {
      canvas: { input_ports: [{ id: 'input', schema: { type: 'table' }, data_kind: 'table' }] },
      outputs: [{ id: 'output', schema: { type: 'number' }, data_kind: 'number' }],
    } as never;
    const ports = nodePorts(capability);
    expect(ports.inputs[0].typeKey).toBe('table');
    expect(ports.outputs[0].typeKey).toBe('number');
    expect(parameterTypeKey({ schema: { type: 'array', items: { type: 'string' } } } as never)).toBe('array<string>');
  });

  it('keeps schema shapes deterministic and maps type styling', () => {
    expect(schemaShape({ type: 'object', properties: { b: { type: 'string' }, a: { type: 'number' } } })).toContain(
      '"a"',
    );
    expect(typeClass('table')).toBe('sf-port-type-table');
    expect(typeIcon('double')).toBe('fa-solid fa-hashtag');
    expect(portTypeKey({ id: 'value', label: 'Value', typeKey: 'unknown' })).toBe('unknown');
  });

  it('preserves top-level output_ports returned by the capability endpoint', () => {
    const ports = nodePorts({ output_ports: [{ id: 'result', label: 'Result', direction: 'output' }] } as never);
    expect(ports.outputs.map((port) => port.id)).toEqual(['result']);
  });

  it('keeps optional input ports when capability shapes expose both input arrays', () => {
    const ports = nodePorts({
      inputs: [{ id: 'analysesTable', direction: 'input', optional: false }],
      input_ports: [
        { id: 'analysesTable', direction: 'input', optional: false },
        { id: 'spectraHeadersTable', direction: 'input', optional: true },
      ],
    } as never);
    expect(ports.inputs.map((port) => port.id)).toEqual(['analysesTable', 'spectraHeadersTable']);
    expect(ports.inputs[1].required).toBe(false);
  });
});
