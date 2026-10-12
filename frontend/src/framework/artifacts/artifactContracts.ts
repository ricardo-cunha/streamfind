export function localArtifactContract(value: string): string {
  return value.split('#').at(-1)?.split(':').at(-1) ?? value;
}

export function artifactContractMatches(left: string, right: string): boolean {
  return left === right || localArtifactContract(left) === localArtifactContract(right);
}
