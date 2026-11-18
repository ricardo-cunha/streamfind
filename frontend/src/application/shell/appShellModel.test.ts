import { describe, expect, it } from 'vitest';
import { projectContainerPath } from './appShellModel';

describe('projectContainerPath', () => {
  it('adds the .sf suffix under a selected parent', () => {
    expect(projectContainerPath('C:\\projects\\', 'river-screening')).toBe('C:/projects/river-screening.sf');
  });

  it('preserves an existing suffix and names containing dots or spaces', () => {
    expect(projectContainerPath('/data/projects', 'river.screening run.sf')).toBe('/data/projects/river.screening run.sf');
  });

  it('supports the filesystem root', () => {
    expect(projectContainerPath('/', 'demo')).toBe('/demo.sf');
  });

  it('rejects path components in the project name', () => {
    expect(() => projectContainerPath('/data', '../outside')).toThrow('single folder name');
  });
});
