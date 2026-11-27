export function localArtifactContract(value: string): string {
  return value.split('#').at(-1)?.split(':').at(-1) ?? value;
}

export function artifactContractMatches(left: string, right: string): boolean {
  if (!left || !right) return false;
  // Qualified semantic identifiers are opaque IRIs.  Never compare their
  // local names: two plugins are allowed to publish the same local name.
  if (isQualifiedArtifactContract(left) || isQualifiedArtifactContract(right)) return left === right;
  return left === right;
}

export function isQualifiedArtifactContract(value: string): boolean {
  return value.includes('#') || /^[A-Za-z][A-Za-z0-9+.-]*:[^\s]+$/.test(value);
}
